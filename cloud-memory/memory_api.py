"""小玖长期记忆云端 API 最小示例。

将此文件部署在 Ubuntu ECS 云服务器上。它使用服务器本机的 SQLite，
并提供与 ``xiaojiu_cloud_memory_client.cpp`` 对应的接口。

必需环境变量：
  XJ_DEVICE_TOKEN  token shared by the device and this API
  XJ_DB_PATH       optional SQLite path (default: /opt/xiaojiu-memory/data/memory.db)

本机测试命令：
  python3 -m venv .venv
  . .venv/bin/activate
  pip install fastapi uvicorn
  XJ_DEVICE_TOKEN='replace-with-a-long-random-token' \
    uvicorn 云端记忆服务器示例:app --host 0.0.0.0 --port 8080

配置 ESP32 之前，必须在 API 前部署 HTTPS（例如使用 Caddy）。
不要把 SQLite 文件或数据库端口暴露到公网。
"""

from __future__ import annotations

import hmac
import os
import sqlite3
from contextlib import closing
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, Header, HTTPException, Query
from pydantic import BaseModel, Field

APP = FastAPI(title="Xiaojiu Memory API", version="0.1.0")
# Lowercase alias keeps the common ``uvicorn module:app`` command convenient.
app = APP
DB_PATH = Path(os.getenv("XJ_DB_PATH", "/opt/xiaojiu-memory/data/memory.db"))
DEVICE_TOKEN = os.getenv("XJ_DEVICE_TOKEN", "")


class RecallRequest(BaseModel):
    user_id: str = Field(min_length=1, max_length=128)
    query: str = Field(default="", max_length=512)


class UpsertRequest(BaseModel):
    user_id: str = Field(min_length=1, max_length=128)
    key: str = Field(min_length=1, max_length=64)
    value: str = Field(min_length=1, max_length=512)
    source: str = Field(default="explicit_user_request", max_length=64)


class ForgetRequest(BaseModel):
    user_id: str = Field(min_length=1, max_length=128)
    key: str = Field(min_length=1, max_length=64)


def now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def database() -> sqlite3.Connection:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(DB_PATH, timeout=5)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA journal_mode=WAL")
    db.execute("PRAGMA busy_timeout=5000")
    return db


def init_database() -> None:
    with closing(database()) as db:
        db.execute(
            """CREATE TABLE IF NOT EXISTS memories (
                user_id TEXT NOT NULL,
                memory_key TEXT NOT NULL,
                memory_value TEXT NOT NULL,
                source TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                PRIMARY KEY (user_id, memory_key)
            )"""
        )
        db.commit()


def require_token(authorization: Optional[str]) -> None:
    if not DEVICE_TOKEN:
        raise HTTPException(status_code=503, detail="XJ_DEVICE_TOKEN is not configured")
    prefix = "Bearer "
    supplied = authorization[len(prefix):].strip() if authorization and authorization.startswith(prefix) else ""
    if not supplied or not hmac.compare_digest(supplied, DEVICE_TOKEN):
        raise HTTPException(status_code=401, detail="invalid device token")


@APP.on_event("startup")
def startup() -> None:
    init_database()


@APP.get("/healthz")
def healthz() -> dict[str, str]:
    return {"status": "ok"}


@APP.post("/v1/memory/recall")
def recall(body: RecallRequest, authorization: Optional[str] = Header(default=None)) -> dict[str, str]:
    require_token(authorization)
    # 首版保持简单可控：最多返回 20 条明确偏好。
    # 后续有大量事件记忆时，再考虑向量或全文检索排序。
    with closing(database()) as db:
        rows = db.execute(
            "SELECT memory_key, memory_value FROM memories WHERE user_id = ? "
            "ORDER BY updated_at DESC LIMIT 20", (body.user_id,)
        ).fetchall()
    lines = [f"{row['memory_key']}: {row['memory_value']}" for row in rows]
    return {"memory_context": "；".join(lines)[:3072]}


@APP.post("/v1/memory/upsert")
def upsert(body: UpsertRequest, authorization: Optional[str] = Header(default=None)) -> dict[str, bool]:
    require_token(authorization)
    with closing(database()) as db:
        db.execute(
            "INSERT INTO memories(user_id, memory_key, memory_value, source, updated_at) "
            "VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(user_id, memory_key) DO UPDATE SET memory_value=excluded.memory_value, "
            "source=excluded.source, updated_at=excluded.updated_at",
            (body.user_id, body.key, body.value, body.source, now()),
        )
        db.commit()
    return {"ok": True}


@APP.post("/v1/memory/forget")
def forget(body: ForgetRequest, authorization: Optional[str] = Header(default=None)) -> dict[str, bool]:
    require_token(authorization)
    with closing(database()) as db:
        db.execute("DELETE FROM memories WHERE user_id = ? AND memory_key = ?", (body.user_id, body.key))
        db.commit()
    return {"ok": True}


@APP.get("/v1/memory/list")
def list_memories(user_id: str = Query(min_length=1, max_length=128), authorization: Optional[str] = Header(default=None)) -> dict[str, list[dict[str, str]]]:
    require_token(authorization)
    with closing(database()) as db:
        rows = db.execute(
            "SELECT memory_key, memory_value, source, updated_at FROM memories "
            "WHERE user_id = ? ORDER BY updated_at DESC", (user_id,)
        ).fetchall()
    return {"memories": [dict(row) for row in rows]}

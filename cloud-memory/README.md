# 小玖云端长期记忆示例

本目录包含两部分：

- `memory_api.py`：部署在 Ubuntu 云服务器的 FastAPI + SQLite 服务端。
- `xiaojiu_cloud_memory_client.cpp`：供 ESP-IDF 固件调用的 HTTPS 客户端参考代码。

这只是可审阅的最小样例，尚未接入小玖正式固件，也未部署到实际云服务器。服务端负责访问数据库；ESP32 只请求 HTTPS API，不能直接连接 SQLite 或保存数据库密码。

## 当前接口

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| `GET` | `/healthz` | 服务健康检查 |
| `POST` | `/v1/memory/recall` | 读取用户记忆摘要 |
| `POST` | `/v1/memory/upsert` | 新增或更新单条记忆 |
| `POST` | `/v1/memory/forget` | 删除指定记忆 |
| `GET` | `/v1/memory/list?user_id=...` | 管理端查看用户记忆 |

写入记忆前必须取得用户明确同意。生产环境还需将设备身份与 `user_id` 绑定，添加速率限制、备份、审计与隐私删除机制。当前共享 Bearer token 仅用于原型验证，不适合作为多设备生产认证方案。

## Ubuntu ECS 快速部署

先将 `memory_api.py` 上传到服务器，例如 `/opt/xiaojiu-memory/memory_api.py`，然后执行：

```bash
sudo apt update
sudo apt install -y python3 python3-venv
sudo mkdir -p /opt/xiaojiu-memory/data
sudo chown -R "$USER":"$USER" /opt/xiaojiu-memory
cd /opt/xiaojiu-memory
python3 -m venv .venv
. .venv/bin/activate
pip install fastapi uvicorn
```

生成并保管设备令牌：

```bash
python3 -c 'import secrets; print(secrets.token_urlsafe(32))'
```

本机临时启动：

```bash
export XJ_DEVICE_TOKEN='替换为刚生成的随机令牌'
uvicorn memory_api:app --host 127.0.0.1 --port 8080
```

另开终端检查：

```bash
curl http://127.0.0.1:8080/healthz
```

生产访问应配置域名和 HTTPS 反向代理（例如 Caddy），将公网 HTTPS 请求转发到 `127.0.0.1:8080`。阿里云安全组只开放必要端口 `22`、`80`、`443`；应用端口 `8080` 与数据库端口不得直接开放公网。

SQLite 默认文件位置：

```text
/opt/xiaojiu-memory/data/memory.db
```

请安排定期加密备份，并实际演练恢复。不要把设备令牌提交到仓库或写死在固件源代码里。

## ESP-IDF 客户端接入

客户端文件目前没有加入工程构建。接入前需要：

1. 将客户端源码纳入 ESP-IDF 构建，并确认工程启用了 HTTPS、证书包和 cJSON。
2. 在单独的低优先级 FreeRTOS 工作任务里调用 `Recall`、`Upsert`、`Forget`。
3. 将服务器 HTTPS 域名、设备令牌和稳定的用户标识从安全配置中读取。
4. 创建新实时会话时，把 `Recall` 返回的短摘要加入会话指令；超时则跳过记忆继续对话。
5. 只在用户明确要求记忆且内容校验通过后调用 `Upsert`。

当前 C++ 示例使用同步 HTTP 调用，超时设置为 1.8 秒，因此尤其不能放到音频、显示或唤醒任务。正式接入前应在目标 ESP-IDF 版本上编译，并验证证书校验、错误重试、断网降级和用户数据隔离。

## 后续扩展建议

一台设备、少量结构化偏好可先用 SQLite。增加设备数量或需要托管备份、高可用时，再迁移到云数据库 MySQL。大量历史事件需要语义回忆时，再评估全文检索或向量数据库；不要一开始就采购高规格数据库。

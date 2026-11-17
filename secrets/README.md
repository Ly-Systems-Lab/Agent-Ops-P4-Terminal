# secrets/ —— MQTT 客户端证书

Hub 通过 **MQTT over TLS + 双向认证**连接局域网里的 broker。构建时，本目录下的三个文件会被
编译进固件（`components/brookesia_hub_service/tools/gen_certs_header.py`）。

| 文件 | 用途 |
|---|---|
| `ca.crt` | 校验 broker 的 CA 证书 |
| `p4.crt` | 本设备的客户端证书 |
| `p4.key` | 本设备的客户端私钥 |

> ⚠️ **`p4.key` 是私钥。** 本目录除本文件外的内容都写在 `.gitignore` 里，不会被提交。
> 请不要把真实证书提交到任何仓库。

## 没有证书也能编译

三个文件缺失时，构建照常通过，只是固件里 `HUB_CERTS_PRESENT` 为 `0` ——
Hub 顶栏会显示「缺证书」，MQTT 连接不会发起。这样可以直接先烧录、把其它功能跑起来。

## 生成一套（示例）

在 broker 所在主机上执行。把 `broker.example.lan` 换成实际使用的主机名或 IP。

```bash
# 1. 自签 CA（10 年）
openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
  -keyout ca.key -out ca.crt -subj "/CN=Local MQTT CA"

# 2. 客户端密钥与证书请求
openssl req -newkey rsa:2048 -sha256 -nodes \
  -keyout p4.key -out p4.csr -subj "/CN=p4-panel"

# 3. 用 CA 签发
openssl x509 -req -in p4.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out p4.crt -days 3650 -sha256
```

把 `ca.crt`、`p4.crt`、`p4.key` 复制到本目录，然后重新构建。

## broker 侧

Mosquitto 的对应配置大致如下（`mosquitto.conf`）：

```conf
listener 8883
cafile   /etc/mosquitto/ca.crt
certfile /etc/mosquitto/server.crt
keyfile  /etc/mosquitto/server.key
require_certificate true

# 1883 明文口只绑回环，不要暴露到局域网
listener 1883 127.0.0.1
allow_anonymous false
password_file /etc/mosquitto/passwd
acl_file      /etc/mosquitto/acl
```

主题与报文格式见 [`docs/mqtt-contract.md`](../docs/mqtt-contract.md)。
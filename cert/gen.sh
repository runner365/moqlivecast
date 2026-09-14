#!/bin/bash
# 生成两段式证书链，符合 Chrome WebTransport(QUIC) 证书验证要求。
#
# 结构：
#   ca_cert.pem    —— 自签 CA（需导入系统信任库）
#   server_cert.pem—— 由 CA 签发的 server 证书，含 SAN + serverAuth
#   server_key.pem —— server 私钥
#
# 关键：server_cert.pem 里拼入 CA 证书，使服务端发送完整链
# （SSL_CTX_use_certificate_file 会读第一个证书为 leaf，其余为链）。
set -e
cd "$(dirname "$0")"

# 1. 自签 CA（导入系统信任库的就是这个）
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out ca_key.pem
openssl req -new -x509 -key ca_key.pem -out ca_cert.pem -days 3650 \
    -subj "/CN=WT Test CA" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"

# 2. server 私钥 + CSR
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out server_key.pem
openssl req -new -key server_key.pem -out server.csr \
    -subj "/CN=127.0.0.1"

# 3. 用 CA 签发 server 证书，含浏览器要求的所有扩展
openssl x509 -req -in server.csr -CA ca_cert.pem -CAkey ca_key.pem \
    -CAcreateserial -out server_cert.pem -days 365 \
    -extfile <(cat <<EOF
subjectAltName=DNS:localhost,IP:127.0.0.1
extendedKeyUsage=serverAuth
keyUsage=digitalSignature,keyEncipherment
basicConstraints=CA:FALSE
EOF
)

rm -f server.csr

# 4. 把 CA 证书拼进 server_cert.pem（完整链）
cat ca_cert.pem >> server_cert.pem

echo "certificates generated:"
ls -la ca_cert.pem ca_key.pem server_cert.pem server_key.pem
echo ""
echo "信任 CA（导入系统信任库）："
echo "  sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain ca_cert.pem"

#!/bin/bash
# 5% loss proxy test script
set -e

PROXY_PORT_FACE=3443
PROXY_PORT_BACK=3444
SERVER_PORT=4434
PROXY_LOG=/tmp/udp_jitter_proxy.log

echo "=== Kill old processes ==="
pkill -9 -f wt_echo_server 2>/dev/null || true
pkill -9 -f udp_jitter_proxy 2>/dev/null || true
pkill -9 -f wt_stress_test 2>/dev/null || true
sleep 2

echo "=== Start echo server :$SERVER_PORT ==="
cd /Users/wei.shi/Documents/code/cc_code/moq
./build/wt_echo_server cert/server_cert.pem cert/server_key.pem 127.0.0.1 $SERVER_PORT > /tmp/wt_echo_server.log 2>&1 &
sleep 2

echo "=== Start proxy :$PROXY_PORT_FACE → :$PROXY_PORT_BACK → :$SERVER_PORT ==="
./udp_jitter_proxy --loss 0.05 --reorder 0 \
    --face-port $PROXY_PORT_FACE --back-port $PROXY_PORT_BACK \
    --server-port $SERVER_PORT > $PROXY_LOG 2>&1 &
sleep 2

echo "=== Start stress test (proxy port $PROXY_PORT_FACE) ==="
rm -f /tmp/wt_stress_test.log
./build/wt_stress_test --ip 127.0.0.1 --port $PROXY_PORT_FACE --size 2000 --rounds 100 --timeout 30 2>&1
RET=$?

echo ""
echo "========================================="
echo "  2KB 100轮 5%丢包 Proxy 测试日志统计"
echo "========================================="
echo "CONNECTED:    $(grep -c 'CONNECTED' /tmp/wt_stress_test.log)"
echo "OK (通过):    $(grep -c 'OK ' /tmp/wt_stress_test.log)"
echo "TIMEOUT:      $(grep -c 'TIMEOUT' /tmp/wt_stress_test.log)"
echo "WRITE FAIL:   $(grep -c 'WRITE FAIL' /tmp/wt_stress_test.log)"
echo "BAD (数据错):  $(grep -c 'got=' /tmp/wt_stress_test.log)"
grep "=== PASS\|=== FAIL" /tmp/wt_stress_test.log || echo "(未完成)"
echo "CLOSED:"
grep "CLOSED" /tmp/wt_stress_test.log | tail -1
echo "Proxy drops:"
grep -c "DROP" $PROXY_LOG

# Cleanup
pkill -9 -f wt_echo_server 2>/dev/null || true
pkill -9 -f udp_jitter_proxy 2>/dev/null || true
echo ""
echo "exit: $RET"

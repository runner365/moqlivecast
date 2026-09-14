package main

import (
	"context"
	"crypto/tls"
	"fmt"
	"io"
	"github.com/quic-go/quic-go"
)

func main() {
	cert, _ := tls.LoadX509KeyPair("../../cert/server_cert.pem", "../../cert/server_key.pem")
	tlsConf := &tls.Config{Certificates: []tls.Certificate{cert}, NextProtos: []string{"quic-echo"}}
	listener, _ := quic.ListenAddr("127.0.0.1:4433", tlsConf, nil)
	fmt.Println("[go-server] listening on 127.0.0.1:4433")
	for {
		conn, err := listener.Accept(context.Background())
		if err != nil { continue }
		go handleConn(conn)
	}
}

func handleConn(conn *quic.Conn) {
	stream, err := conn.AcceptStream(context.Background())
	if err != nil { return }

	// quic-go 官方 echo 模式: io.Copy 循环
	n, err := io.Copy(stream, stream)
	fmt.Printf("[go-server] echoed %d bytes (err=%v)\n", n, err)
}

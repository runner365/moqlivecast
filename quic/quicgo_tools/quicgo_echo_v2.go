package main

import (
	"context"
	"crypto/tls"
	"fmt"
	"io"
	"os"
	"github.com/quic-go/quic-go"
)

func main() {
	cert, _ := tls.LoadX509KeyPair("../../cert/server_cert.pem", "../../cert/server_key.pem")
	tlsConf := &tls.Config{Certificates: []tls.Certificate{cert}, NextProtos: []string{"quic-echo"}}
	listener, _ := quic.ListenAddr("127.0.0.1:4433", tlsConf, nil)
	fmt.Println("[go-server] listening")
	for {
		conn, _ := listener.Accept(context.Background())
		fmt.Printf("[go-server] new conn from %s\n", conn.RemoteAddr())
		go func(c *quic.Conn) {
			stream, _ := c.AcceptStream(context.Background())
			// 简单 echo: 读完所有数据，写回，等对方确认后 Close 触发 flush
			data, _ := io.ReadAll(stream)
			if len(data) > 0 {
				fmt.Printf("[go-server] got %d bytes\n", len(data))
				stream.Write(data)
				fmt.Printf("[go-server] wrote %d bytes\n", len(data))
			}
			stream.Close() // FIN triggers flush of pending data
		}(conn)
	}
}

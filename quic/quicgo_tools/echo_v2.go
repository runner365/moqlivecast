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
	fmt.Println("[go-server] listening")
	for {
		conn, _ := listener.Accept(context.Background())
		go func(c *quic.Conn) {
			for {
				stream, err := c.AcceptStream(context.Background())
				if err != nil { return }
				// 读-写-Close 模式: Close 触发 FIN+flush
				data, _ := io.ReadAll(stream)
				if len(data) > 0 {
					fmt.Printf("[go] rx %d: %s\n", len(data), string(data))
					stream.Write(data)
					fmt.Printf("[go] tx %d bytes\n", len(data))
				}
				stream.Close()
			}
		}(conn)
	}
}

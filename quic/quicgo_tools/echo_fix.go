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
	fmt.Println("[go] listening")
	for {
		conn, _ := listener.Accept(context.Background())
		go func(c *quic.Conn) {
			stream, _ := c.AcceptStream(context.Background())
			buf := make([]byte, 65536)
			for {
				n, err := stream.Read(buf)
				if n > 0 {
					fmt.Printf("[go] rx %d: %s\n", n, string(buf[:n]))
					stream.Write(buf[:n])
				}
				if err == io.EOF { break }
				if err != nil { return }
			}
			// 读完所有数据后 Close 刷新写缓冲
			stream.Close()
			fmt.Println("[go] done")
		}(conn)
	}
}

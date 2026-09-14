package main

import (
	"context"
	"crypto/tls"
	"fmt"
	"github.com/quic-go/quic-go"
)

func main() {
	tlsConf := &tls.Config{InsecureSkipVerify: true, NextProtos: []string{"quic-echo"}}
	conn, _ := quic.DialAddr(context.Background(), "127.0.0.1:4433", tlsConf, nil)
	stream, _ := conn.OpenStreamSync(context.Background())
	msg := []byte("Hello")
	stream.Write(msg)
	stream.Close() // 只发一次
	fmt.Println("sent 5 bytes + FIN")

	buf := make([]byte, 65536)
	for {
		n, err := stream.Read(buf)
		if n > 0 { fmt.Printf("got echo: %s\n", string(buf[:n])) }
		if err != nil { fmt.Printf("read done: %v\n", err); break }
	}
}

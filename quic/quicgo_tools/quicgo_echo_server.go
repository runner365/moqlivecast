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
	certFile := "../../cert/server_cert.pem"
	keyFile := "../../cert/server_key.pem"
	addr := "127.0.0.1:4433"

	if len(os.Args) >= 3 {
		certFile = os.Args[1]
		keyFile = os.Args[2]
	}
	if len(os.Args) >= 4 {
		addr = os.Args[3]
	}
	if len(os.Args) >= 5 {
		addr = addr + ":" + os.Args[4]
	}

	cert, err := tls.LoadX509KeyPair(certFile, keyFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "load cert failed: %v\n", err)
		os.Exit(1)
	}

	tlsConf := &tls.Config{
		Certificates: []tls.Certificate{cert},
		NextProtos:   []string{"quic-echo"},
	}

	listener, err := quic.ListenAddr(addr, tlsConf, nil)
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen failed: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("[go-server] listening on %s\n", addr)

	for {
		conn, err := listener.Accept(context.Background())
		if err != nil {
			fmt.Fprintf(os.Stderr, "accept failed: %v\n", err)
			continue
		}
		fmt.Printf("[go-server] new connection from %s\n", conn.RemoteAddr())
		go handleConn(conn)
	}
}

func handleConn(conn *quic.Conn) {
	stream, err := conn.AcceptStream(context.Background())
	if err != nil {
		return
	}

	buf := make([]byte, 65536)
	for {
		n, err := stream.Read(buf)
		if n > 0 {
			fmt.Printf("[go-server] received %d bytes: %s\n", n, string(buf[:n]))
			_, werr := stream.Write(buf[:n])
			if werr != nil {
				fmt.Fprintf(os.Stderr, "write failed: %v\n", werr)
				break
			}
			fmt.Printf("[go-server] echoed %d bytes\n", n)
		}
		if err != nil {
			if err != io.EOF {
				fmt.Fprintf(os.Stderr, "read failed: %v\n", err)
			}
			break
		}
	}
}

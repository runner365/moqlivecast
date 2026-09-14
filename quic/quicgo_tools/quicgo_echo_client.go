package main

import (
	"context"
	"crypto/tls"
	"fmt"
	"io"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/quic-go/quic-go"
)

func main() {
	addr := "127.0.0.1:4433"
	if len(os.Args) >= 2 {
		addr = os.Args[1]
	}
	if len(os.Args) >= 3 {
		addr = addr + ":" + os.Args[2]
	}

	tlsConf := &tls.Config{
		InsecureSkipVerify: true,
		NextProtos:         []string{"quic-echo"},
	}

	fmt.Printf("[go-client] connecting to %s\n", addr)
	conn, err := quic.DialAddr(context.Background(), addr, tlsConf, nil)
	if err != nil {
		fmt.Fprintf(os.Stderr, "dial failed: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("[go-client] QUIC handshake complete!")

	stream, err := conn.OpenStreamSync(context.Background())
	if err != nil {
		fmt.Fprintf(os.Stderr, "open stream failed: %v\n", err)
		os.Exit(1)
	}

	done := make(chan struct{})
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	// 接收协程
	go func() {
		buf := make([]byte, 65536)
		for {
			n, err := stream.Read(buf)
			if n > 0 {
				fmt.Printf("[go-client] received %d bytes: %s\n", n, string(buf[:n]))
			}
			if err != nil {
				if err != io.EOF {
					fmt.Fprintf(os.Stderr, "read failed: %v\n", err)
				}
				return
			}
		}
	}()

	// 发送协程：每 2 秒发送一次
	go func() {
		counter := 0
		ticker := time.NewTicker(50 * time.Millisecond)
		defer ticker.Stop()

		// 立即发送第一条
		sendMsg := func() {
			counter++
			msg := fmt.Sprintf("Hello QUIC! #%d", counter)
			n, err := stream.Write([]byte(msg))
			if err != nil {
				fmt.Fprintf(os.Stderr, "write failed: %v\n", err)
				close(done)
				return
			}
			fmt.Printf("[go-client] sent '%s' (%d bytes)\n", msg, n)
		}

		sendMsg()
		for {
			select {
			case <-ticker.C:
				sendMsg()
			case <-done:
				return
			}
		}
	}()

	fmt.Println("[go-client] press Ctrl+C to stop")

	select {
	case <-sigCh:
		fmt.Println("[go-client] shutting down...")
	case <-done:
	}

	conn.CloseWithError(0, "bye")
	fmt.Println("[go-client] connection closed")
}

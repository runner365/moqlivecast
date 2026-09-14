package main

import (
	"context"
	"crypto/tls"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"time"

	"github.com/quic-go/quic-go"
	"github.com/quic-go/webtransport-go"
)

/* Self-validating stress test client for WebTransport.
 * Each round: write payload with round-keyed byte pattern,
 * then read echo with byte-by-byte validation.
 * Per-round deadline enforces timeout. */

var (
	addr    = flag.String("addr", "127.0.0.1:4433", "server address")
	size    = flag.Int("size", 2000, "payload bytes per round")
	rounds  = flag.Int("rounds", 100, "number of rounds")
	timeout = flag.Int("timeout", 30, "per-round timeout (seconds)")
)

func main() {
	flag.Parse()

	logFile, err := os.Create("/tmp/wt_go_stress.log")
	if err != nil {
		log.Fatalf("create log: %v", err)
	}
	defer logFile.Close()
	lg := log.New(io.MultiWriter(os.Stderr, logFile), "", log.LstdFlags|log.Lmicroseconds)

	url := fmt.Sprintf("https://%s/webtransport", *addr)
	lg.Printf("TARGET=%s PAYLOAD=%dB ROUNDS=%d TIMEOUT=%ds", *addr, *size, *rounds, *timeout)

	d := &webtransport.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
		QUICConfig: &quic.Config{
			EnableDatagrams:                  true,
			EnableStreamResetPartialDelivery: true,
			MaxIdleTimeout:                   120 * time.Second,
		},
	}

	rsp, wtConn, err := d.Dial(context.Background(), url, nil)
	if err != nil {
		lg.Fatalf("DIAL FAIL: %v", err)
	}
	lg.Printf("CONNECTED (status %d)", rsp.StatusCode)
	defer wtConn.CloseWithError(0, "")

	stream, err := wtConn.OpenStreamSync(context.Background())
	if err != nil {
		lg.Fatalf("OPEN STREAM FAIL: %v", err)
	}
	defer stream.Close()
	lg.Printf("STREAM_OPEN")

	sendBuf := make([]byte, *size)
	recvBuf := make([]byte, *size)
	totalErrors := 0
	testStart := time.Now()

	for r := 1; r <= *rounds; r++ {
		// Fill: byte[i] = (round * 37 + i) & 0xFF
		for i := 0; i < *size; i++ {
			sendBuf[i] = byte((r*37 + i) & 0xFF)
		}
		clearBuf(recvBuf)

		rStart := time.Now()

		// Write
		if _, err := stream.Write(sendBuf); err != nil {
			lg.Fatalf("R%d WRITE FAIL: %v", r, err)
		}

		// Read echo with timeout
		roff := 0
		errs := 0
		errorRound := false
		stream.SetReadDeadline(time.Now().Add(time.Duration(*timeout) * time.Second))
		for roff < *size {
			n, err := stream.Read(recvBuf[roff:])
			if err != nil {
				if !errorRound {
					lg.Printf("R%d/%d TIMEOUT got=%d/%d err=%v", r, *rounds, roff, *size, err)
					errorRound = true
				}
				break
			}
			for i := 0; i < n; i++ {
				exp := byte((r*37 + roff + i) & 0xFF)
				if recvBuf[roff+i] != exp {
					errs++
				}
			}
			roff += n
		}
		stream.SetReadDeadline(time.Time{})

		rElapsedMs := time.Since(rStart).Milliseconds()
		if errs == 0 && roff >= *size {
			mbps := float64(*size*8) / 1e6 / (float64(rElapsedMs) / 1000.0)
			lg.Printf("R%d/%d OK  %dms %.2fMbps", r, *rounds, rElapsedMs, mbps)
		} else {
			totalErrors += errs
			lg.Printf("R%d/%d FAIL  %d errs %d/%dB", r, *rounds, errs, roff, *size)
		}
	}

	totalElapsed := time.Since(testStart).Seconds()
	totalBytes := uint64(*size) * uint64(*rounds)
	avgMbps := float64(totalBytes*8) / 1e6 / totalElapsed
	result := "PASS"
	if totalErrors > 0 {
		result = "FAIL"
	}
	lg.Printf("=== %s  %dB  %.1fs  %.2fMbps  err=%d ===", result, totalBytes, totalElapsed, avgMbps, totalErrors)
}

func clearBuf(b []byte) {
	for i := range b {
		b[i] = 0
	}
}

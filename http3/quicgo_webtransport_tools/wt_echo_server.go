package main

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"flag"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"time"

	"github.com/quic-go/quic-go/http3"
	"github.com/quic-go/webtransport-go"
)

var lg *log.Logger

func main() {
	addr := flag.String("addr", "127.0.0.1:4433", "listen address")
	certFile := flag.String("cert", "", "TLS cert file (generated if empty)")
	keyFile := flag.String("key", "", "TLS key file (generated if empty)")
	flag.Parse()

	logFile, err := os.Create("/tmp/wt_go_echo_server.log")
	if err != nil {
		log.Fatalf("cannot create log file: %v", err)
	}
	defer logFile.Close()
	lg = log.New(io.MultiWriter(os.Stderr, logFile), "", log.LstdFlags|log.Lmicroseconds)

	tlsConf, err := loadOrGenerateTLS(*certFile, *keyFile)
	if err != nil {
		lg.Fatalf("TLS setup: %v", err)
	}

	s := &webtransport.Server{
		H3: &http3.Server{Addr: *addr, TLSConfig: tlsConf},
		CheckOrigin: func(r *http.Request) bool { return true },
	}

	s.H3.Handler = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		session, err := s.Upgrade(w, r)
		if err != nil {
			lg.Printf("Upgrade: %v", err)
			w.WriteHeader(http.StatusInternalServerError)
			return
		}
		lg.Printf("SESSION from %s", r.RemoteAddr)
		go handleSession(session)
	})

	lg.Printf("LISTENING https://%s", *addr)
	if err := s.ListenAndServe(); err != nil {
		lg.Fatalf("server: %v", err)
	}
}

func handleSession(session *webtransport.Session) {
	for {
		stream, err := session.AcceptStream(context.Background())
		if err != nil {
			return
		}
		go handleStream(stream)
	}
}

func handleStream(stream *webtransport.Stream) {
	defer stream.Close()
	buf := make([]byte, 65536)
	for {
		n, err := stream.Read(buf)
		if err != nil {
			if err != io.EOF {
				lg.Printf("stream %d read: %v", stream.StreamID(), err)
			}
			return
		}
		if _, err := stream.Write(buf[:n]); err != nil {
			lg.Printf("stream %d write: %v", stream.StreamID(), err)
			return
		}
	}
}

func loadOrGenerateTLS(certFile, keyFile string) (*tls.Config, error) {
	if certFile != "" && keyFile != "" {
		cert, err := tls.LoadX509KeyPair(certFile, keyFile)
		if err != nil {
			return nil, fmt.Errorf("load cert: %w", err)
		}
		return &tls.Config{Certificates: []tls.Certificate{cert}, NextProtos: []string{"h3"}}, nil
	}

	lg.Printf("generating self-signed TLS cert...")
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return nil, err
	}
	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		NotBefore:    time.Now(),
		NotAfter:     time.Now().Add(365 * 24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1")},
	}
	certDER, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		return nil, err
	}
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: certDER})
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)})
	cert, err := tls.X509KeyPair(certPEM, keyPEM)
	if err != nil {
		return nil, err
	}
	return &tls.Config{Certificates: []tls.Certificate{cert}, NextProtos: []string{"h3"}}, nil
}

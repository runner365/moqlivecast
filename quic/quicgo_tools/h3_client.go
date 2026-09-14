package main

import (
	"crypto/tls"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"

	"github.com/quic-go/quic-go/http3"
)

func main() {
	addr := "https://127.0.0.1:4433"
	if len(os.Args) >= 2 {
		addr = os.Args[1]
	}

	tr := &http3.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
	}
	defer tr.Close()

	client := &http.Client{Transport: tr}

	passed, failed := 0, 0

	/* ── Test 1: GET /get/hello?name=alex&age=23 ─── */
	fmt.Println("========================================")
	fmt.Println("Test 1: GET /get/hello?name=alex&age=23")
	fmt.Println("========================================")
	if runGet(client, addr, "/get/hello?name=alex&age=23") {
		passed++
	} else {
		failed++
	}

	/* ── Test 2: POST /post/hello (json body) ──── */
	fmt.Println("\n========================================")
	fmt.Println("Test 2: POST /post/hello (JSON body)")
	fmt.Println("========================================")
	if runPost(client, addr, "/post/hello",
		"application/json",
		`{"name":"alex","age":23}`) {
		passed++
	} else {
		failed++
	}

	/* ── Test 3: POST /post/hello (plain text) ─── */
	fmt.Println("\n========================================")
	fmt.Println("Test 3: POST /post/hello (plain text)")
	fmt.Println("========================================")
	if runPost(client, addr, "/post/hello",
		"text/plain",
		"hello from h3_client.go") {
		passed++
	} else {
		failed++
	}

	/* ── Summary ──────────────────────────────── */
	fmt.Println("\n========================================")
	fmt.Printf("Results: %d passed, %d failed\n", passed, failed)
	fmt.Println("========================================")

	if failed > 0 {
		os.Exit(1)
	}
}

func runGet(client *http.Client, addr, path string) bool {
	url := addr + path
	fmt.Printf("[GET] %s\n", url)

	resp, err := client.Get(url)
	if err != nil {
		fmt.Fprintf(os.Stderr, "  FAIL: %v\n", err)
		return false
	}
	defer resp.Body.Close()
	return printResponse(resp)
}

func runPost(client *http.Client, addr, path, contentType, body string) bool {
	url := addr + path
	fmt.Printf("[POST] %s\n", url)
	fmt.Printf("  Content-Type: %s\n", contentType)
	fmt.Printf("  Body: %s\n", body)

	resp, err := client.Post(url, contentType, strings.NewReader(body))
	if err != nil {
		fmt.Fprintf(os.Stderr, "  FAIL: %v\n", err)
		return false
	}
	defer resp.Body.Close()
	return printResponse(resp)
}

func printResponse(resp *http.Response) bool {
	fmt.Printf("  Status: %s\n", resp.Status)
	for k, vs := range resp.Header {
		for _, v := range vs {
			fmt.Printf("  Header: %s = %s\n", k, v)
		}
	}
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		fmt.Fprintf(os.Stderr, "  FAIL: read body: %v\n", err)
		return false
	}
	fmt.Printf("  Body[%d]:\n%s\n", len(body), string(body))
	return resp.StatusCode >= 200 && resp.StatusCode < 300
}

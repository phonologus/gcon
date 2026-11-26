// The ring and sieve of ../ring.icn and ../sieve.icn in Go, for
// comparison: real goroutines and channels.
//
//	go run . ring [n [m]]
//	go run . sieve [n]
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

func arg(i, def int) int {
	if len(os.Args) > i {
		if n, err := strconv.Atoi(os.Args[i]); err == nil {
			return n
		}
	}
	return def
}

func ring(n, m int) {
	t := time.Now()
	first := make(chan int)
	left := first
	for i := 0; i < n; i++ {
		right := make(chan int)
		go func(l, r chan int) {
			for {
				r <- <-l + 1
			}
		}(left, right)
		left = right
	}
	x := 0
	for i := 0; i < m; i++ {
		first <- x
		x = <-left
	}
	d := time.Since(t)
	fmt.Println("token:", x)
	fmt.Fprintf(os.Stderr, "ring %d x %d: %d ms, %d ns per hand-off\n", n, m, d.Milliseconds(), d.Nanoseconds()/int64(n*m))
}

func sieve(n int) {
	t := time.Now()
	ch := make(chan int)
	go func(ch chan int) {
		for i := 2; ; i++ {
			ch <- i
		}
	}(ch)
	p := 0
	for i := 0; i < n; i++ {
		p = <-ch
		ch1 := make(chan int)
		go func(in, out chan int, p int) {
			for {
				if i := <-in; i%p != 0 {
					out <- i
				}
			}
		}(ch, ch1, p)
		ch = ch1
	}
	fmt.Printf("prime %d: %d\n", n, p)
	fmt.Fprintf(os.Stderr, "sieve %d: %d ms\n", n, time.Since(t).Milliseconds())
}

func main() {
	switch os.Args[1] {
	case "ring":
		os.Args = os.Args[1:]
		ring(arg(1, 1000), arg(2, 1000))
	case "sieve":
		os.Args = os.Args[1:]
		sieve(arg(1, 1000))
	}
}

package main

import (
	"fmt"
	"os"
	"strings"

	"github.com/dop251/goja"
)

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: goja <script>")
		os.Exit(2)
	}
	source, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	vm := goja.New()
	vm.Set("print", func(call goja.FunctionCall) goja.Value {
		parts := make([]string, len(call.Arguments))
		for index, argument := range call.Arguments {
			parts[index] = argument.String()
		}
		fmt.Println(strings.Join(parts, " "))
		return goja.Undefined()
	})

	if _, err := vm.RunScript(os.Args[1], string(source)); err != nil {
		if exception, ok := err.(*goja.Exception); ok {
			fmt.Fprintln(os.Stderr, exception.String())
		} else {
			fmt.Fprintln(os.Stderr, err)
		}
		os.Exit(1)
	}
}

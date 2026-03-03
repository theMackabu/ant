import whisp from './whisp';

whisp`
(fn fib n a b
  (if (= n 0) a
    (fib (- n 1) b (+ a b))))

(let start (performance.now))
(let result (fib 30 0 1))
(let end (performance.now))
(let elapsed (- end start))

(write "fibonacci(30) = " result "\n")
(write "Time: " (. elapsed toFixed 4) " ms (" (. (* elapsed 1000) toFixed 2) " µs)\n"))
`;

import whisp from './whisp';

whisp`(write "running Whisp\n")`;

whisp`
(do
(let n 30)
  (if (<= n 1) n
    (do
      (let a 0)
      (let b 1)
      (let i 2)
      (let temp 0)
      (loop (<= i n)
        (do
          (let temp (+ a b))
          (let a b)
          (let b temp)
          (let i (+ i 1))))
      (write "fib(30): " b "\n"))))
`;

; Emptiness of the intersection of two regexes: a(ba)* only contains words of odd length,
; (\w\w)* only contains words of even length.
(set-logic QF_S)
(set-info :status unsat)

(declare-const x String)

(assert (str.in_re x (re.from_ecma2020 "a(ba)*")))
(assert (str.in_re x ((_ re.from_regex ecma2020) "(\w\w)*")))

(check-sat)

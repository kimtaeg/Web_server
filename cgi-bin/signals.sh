#!/bin/sh
# Reports whether SIGTERM reaches the script (it must not inherit the worker's blocked mask).
result=blocked
trap 'result=delivered' TERM
kill -TERM $$
sleep 0.1
printf 'Content-Type: text/plain\r\n\r\nSIGTERM %s\n' "$result"

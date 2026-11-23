#!/bin/sh
# Never finishes in time: the server kills it after cgi_timeout and answers 504.
sleep 37 # distinctive, so tests can check that no orphan survives the timeout
printf 'Content-Type: text/plain\r\n\r\ntoo late\n'

#!/bin/sh
# Minimal CGI: shows the request as the server passed it in the environment.
printf 'Content-Type: text/plain; charset=utf-8\r\n'
printf 'X-CGI: hello\r\n\r\n'
echo "Hello from CGI (pid $$)"
echo "REQUEST_METHOD=$REQUEST_METHOD"
echo "SCRIPT_NAME=$SCRIPT_NAME"
echo "PATH_INFO=$PATH_INFO"
echo "QUERY_STRING=$QUERY_STRING"
echo "REMOTE_ADDR=$REMOTE_ADDR"
echo "HTTP_USER_AGENT=$HTTP_USER_AGENT"

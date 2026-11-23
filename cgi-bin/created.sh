#!/bin/sh
# Demonstrates the CGI Status and Location headers.
printf 'Status: 201 Created\r\nLocation: /items/42\r\nContent-Type: application/json\r\n\r\n'
printf '{"id": 42}\n'

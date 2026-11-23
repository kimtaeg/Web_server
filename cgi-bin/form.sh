#!/bin/sh
# Reads a POST body of CONTENT_LENGTH bytes from stdin and echoes it back, HTML-escaped.
body=""
if [ -n "$CONTENT_LENGTH" ] && [ "$CONTENT_LENGTH" -gt 0 ]; then
  body=$(head -c "$CONTENT_LENGTH")
fi
escaped=$(printf '%s' "$body" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' -e 's/"/\&quot;/g')
printf 'Content-Type: text/html; charset=utf-8\r\n\r\n'
cat <<HTML
<!doctype html><meta charset="utf-8"><title>form result</title>
<link rel="stylesheet" href="/css/style.css">
<main class="wrap">
<h1>POST received</h1>
<p>method: $REQUEST_METHOD &middot; content-type: $CONTENT_TYPE &middot; bytes: ${CONTENT_LENGTH:-0}</p>
<pre>$escaped</pre>
<p><a href="/">back</a></p>
</main>
HTML

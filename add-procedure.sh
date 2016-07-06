#!/bin/bash

BODY=$(cat << EOF
{
	"name":"hello/world",
	"version":1,
	"code":"throw new Error(123)"
}
EOF
)
echo $BODY
curl -H "Connection: close" -v -XPOST -d "$BODY" "http://127.0.0.1:9500/_f/admin/register-procedure?pretty&hello=world&abc=123&version=1"

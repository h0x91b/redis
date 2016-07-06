#!/bin/bash

BODY=$(cat << EOF
{
}
EOF
)
echo $BODY
curl -H "Connection: close" -v -XPOST -d "$BODY" "http://127.0.0.1:9500/_f/hello/world?pretty"

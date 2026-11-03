#!/bin/sh
# Fetches BTstack (BlueKitchen) at the revision this project was written against.
set -e
cd "$(dirname "$0")"
BTSTACK_REV=9edb5d82e
if [ ! -d btstack ]; then
    git clone https://github.com/bluekitchen/btstack.git btstack
fi
git -C btstack fetch -q origin
git -C btstack checkout -q "$BTSTACK_REV"
echo "BTstack ready at $(git -C btstack log -1 --format='%h %ad' --date=short)"

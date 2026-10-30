#!/bin/sh
# Создаёт самоподписанный сертификат и ключ для сервера в текущем каталоге.
# Использование: scripts/gen-cert.sh [имя_хоста_или_IP ...]
set -eu

if [ -e server.key ] || [ -e server.crt ]; then
    echo "server.key/server.crt уже существуют, ничего не меняю" >&2
    exit 1
fi

san="DNS:localhost,IP:127.0.0.1"
for host in "$@"; do
    case "$host" in
        *[!0-9.]*) san="$san,DNS:$host" ;;
        *)         san="$san,IP:$host" ;;
    esac
done

umask 077
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout server.key -out server.crt -days 3650 \
    -subj "/CN=MeZZenger" -addext "subjectAltName=$san"

echo "Готово: server.crt, server.key"
echo "Отпечаток SHA-256 (сверяйте с тем, что покажет клиент при первом входе):"
openssl x509 -in server.crt -noout -fingerprint -sha256

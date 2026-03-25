#!/bin/bash
# Build and run all tests inside Docker container
# Usage: ./docker-check.sh
#   or:  ./docker-check.sh build   (just build zerc)
#   or:  ./docker-check.sh shell   (interactive shell)

set -e

IMAGE="zer-lang-dev"

# Build the Docker image
docker build -t $IMAGE .

case "${1:-check}" in
    check)
        docker run --rm $IMAGE make check
        ;;
    build)
        docker run --rm $IMAGE make zerc
        ;;
    shell)
        docker run --rm -it $IMAGE bash
        ;;
    *)
        echo "Usage: $0 [check|build|shell]"
        exit 1
        ;;
esac

FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential bash bc bison flex cpio unzip rsync file wget curl ca-certificates \
    xz-utils python3 perl patch git libssl-dev libncurses-dev gawk time \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 1000 builder
USER builder
WORKDIR /build

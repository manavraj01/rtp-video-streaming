# Builds and runs the RTP pipeline on real Linux — needed for tc netem
# (Linux-only) and to match the resume's stated environment.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config make curl ca-certificates \
    libavformat-dev libavcodec-dev libavutil-dev \
    ffmpeg iproute2 tcpdump gdb \
    && rm -rf /var/lib/apt/lists/*

# Debian bookworm's packaged Go is too old for the go 1.22 stdlib
# net/http method+path routing statsservice uses — install upstream Go.
RUN curl -fsSL https://go.dev/dl/go1.22.6.linux-amd64.tar.gz | tar -C /usr/local -xz
ENV PATH="/usr/local/go/bin:${PATH}"

WORKDIR /app
COPY . .
RUN make
RUN cd statsservice && go build -o statsservice .

CMD ["bash"]

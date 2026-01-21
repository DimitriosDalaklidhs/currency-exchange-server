# build stage
FROM gcc:13 AS build
WORKDIR /app

COPY server.c .

# Build with hardening flags, strip symbols to reduce size
RUN gcc -Wall -Wextra -O2 \
    -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -pie \
    -Wl,-z,relro,-z,now \
    server.c -o server \
 && strip server

# ---- runtime stage
FROM debian:bookworm-slim
WORKDIR /app

# Install only what's needed for runtime healthcheck (useful)
RUN apt-get update \
 && apt-get install -y --no-install-recommends netcat-openbsd ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Create non-root user (no login shell)
RUN useradd -m -r -s /usr/sbin/nologin appuser

# Copy binary
COPY --from=build /app/server /app/server

# Create a dedicated writable directory for the DB (avoid writing into /app)
RUN mkdir -p /data \
 && chown -R appuser:appuser /data

# So we point workdir at /data instead (server binary is still in /app).
WORKDIR /data

EXPOSE 8080

# Basic runtime healthcheck (checks port is open)
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD nc -z 127.0.0.1 8080 || exit 1

USER appuser
CMD ["/app/server"]


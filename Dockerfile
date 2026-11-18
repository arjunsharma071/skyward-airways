FROM debian:bookworm-slim AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential ca-certificates libpq-dev libssl-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY backend/ ./backend/
COPY database/ ./database/
COPY frontend/ ./frontend/

RUN mkdir -p /out \
    && g++ -std=c++17 -O2 -Wall -Wextra -pedantic -pthread \
       -DSKYWARD_WITH_POSTGRES \
       $(pkg-config --cflags libpq) \
       backend/main.cpp backend/AirlineReservationSystem.cpp backend/PostgresStore.cpp \
       -o /out/airline_reservation_server \
       $(pkg-config --libs libpq) -lcrypto

FROM debian:bookworm-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl libpq5 libssl3 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /app/data \
    && chown -R 10001:10001 /app

WORKDIR /app
COPY --from=build --chown=10001:10001 /out/airline_reservation_server ./airline_reservation_server
COPY --from=build --chown=10001:10001 /src/frontend ./frontend
COPY --from=build --chown=10001:10001 /src/database ./database

ENV APP_ENV=production \
    DATA_DIRECTORY=/app/data \
    SERVER_HOST=0.0.0.0 \
    SERVER_PORT=8080

EXPOSE 8080
STOPSIGNAL SIGTERM
USER 10001:10001

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD curl --fail --silent http://127.0.0.1:8080/api/health || exit 1

ENTRYPOINT ["/app/airline_reservation_server"]

FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends gcc make libc6-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY src ./src
COPY Makefile .
RUN make all

FROM debian:bookworm-slim
RUN useradd --uid 10001 --create-home bankapp
WORKDIR /app
COPY --from=build /app/build/banking_server /app/banking_server
COPY public ./public
USER bankapp
ENV PORT=10000
EXPOSE 10000
CMD ["/app/banking_server", "/app/public"]

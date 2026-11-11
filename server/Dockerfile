FROM golang:1.25-alpine AS build
WORKDIR /src
COPY go.mod ./
COPY *.go ./
COPY web ./web
RUN CGO_ENABLED=0 go test ./... && go build -trimpath -ldflags="-s -w" -o /dutyon-cloud .

FROM alpine:3.22
RUN addgroup -g 10001 dutyon && adduser -D -u 10001 -G dutyon dutyon \
    && mkdir /data && chown 10001:10001 /data
COPY --from=build /dutyon-cloud /usr/local/bin/dutyon-cloud
USER 10001:10001
WORKDIR /data
EXPOSE 8787
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s \
    CMD wget -q -O /dev/null http://127.0.0.1:8787/healthz || exit 1
ENTRYPOINT ["/usr/local/bin/dutyon-cloud"]
CMD ["--addr", ":8787", "--data", "/data"]

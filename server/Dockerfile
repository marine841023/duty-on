FROM golang:1.27.4-alpine AS build
WORKDIR /src
COPY go.mod ./
COPY *.go ./
COPY web ./web
RUN CGO_ENABLED=0 go test ./... && go build -trimpath -ldflags="-s -w" -o /dutyon-cloud .

FROM scratch
COPY --chown=10001:10001 runtime-data /data
COPY --from=build /dutyon-cloud /usr/local/bin/dutyon-cloud
USER 10001:10001
WORKDIR /data
EXPOSE 8787
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s \
    CMD ["/usr/local/bin/dutyon-cloud", "--healthcheck", "http://127.0.0.1:8787/healthz"]
ENTRYPOINT ["/usr/local/bin/dutyon-cloud"]
CMD ["--addr", ":8787", "--data", "/data"]

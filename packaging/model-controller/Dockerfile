# M8A Step 5 — model-controller sidecar.
#
# Two-stage build: golang:alpine compiles the static binary, then we
# copy it onto docker:cli so the runtime has the docker CLI present
# (the controller shells out to `docker compose ...`). The host's
# docker socket must be bind-mounted at /var/run/docker.sock at run
# time — see packaging/compose/docker-compose.yml for the recommended
# service definition.

FROM golang:1.23-alpine AS build
WORKDIR /src
COPY main.go ./
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" \
    -o /out/model-controller ./

FROM docker:cli
COPY --from=build /out/model-controller /usr/local/bin/model-controller
EXPOSE 9877
ENTRYPOINT ["/usr/local/bin/model-controller"]

FROM golang:1.25-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -o /flp-admin ./cmd/flp-admin

FROM alpine:3.20
RUN apk add --no-cache ca-certificates
COPY --from=build /flp-admin /usr/local/bin/flp-admin
RUN mkdir -p /app/received_files
WORKDIR /app
EXPOSE 5050
ENTRYPOINT ["flp-admin"]
CMD ["--broker", "mosquitto", "--port", "1883"]

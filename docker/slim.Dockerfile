FROM gcc:bullseye as builder

COPY . .
RUN cd ../libjodycode && make && cd ../jdupes && make && make install

FROM debian:bullseye-slim as runner

COPY --from=builder /usr/local/bin/jdupes /usr/local/bin/jdupes

ENTRYPOINT [ "/usr/local/bin/jdupes" ]

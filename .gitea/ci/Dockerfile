FROM archlinux:base-devel@sha256:99e98cdd1c714023ca481b4959c9214415323129530bfc101da2ff50dd9a6106
RUN pacman -Syu --noconfirm --needed cmake ninja qt6-base qt6-tools python git curl \
    && rm -f /var/cache/pacman/pkg/*
WORKDIR /work
ENV HOME=/tmp

"""Structured logging helpers for the control plane."""

from __future__ import annotations

import logging


def get_logger(name: str) -> logging.Logger:
    """Return the package logger with a stable default format."""
    logger = logging.getLogger(name)
    if not logger.handlers:
        handler = logging.StreamHandler()
        handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s"))
        logger.addHandler(handler)
    return logger

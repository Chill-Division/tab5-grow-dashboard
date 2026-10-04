"""Helpers for the Tab5 grow dashboard: 6-hour trend buffers, Home Assistant history
backfill and the chart renderer. Header-only; dashboard.yaml's lambdas call into
grow_dashboard.h, and an empty `grow_dashboard:` entry loads it."""

import esphome.config_validation as cv

CODEOWNERS = ["@ChillingSilence"]
DEPENDENCIES = ["lvgl"]
AUTO_LOAD = ["json"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass

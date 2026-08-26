#!/usr/bin/env python

import os
import sys

# Resolve path configuration
root = os.path.join(os.path.dirname(__file__), "..")
src = os.path.join(root, "pypokergui")
sys.path.append(root)
sys.path.append(src)
# server/poker.py does a bare "import poker_conf" (poker_conf.py lives
# alongside it in pypokergui/server/), which only resolves when
# pypokergui/server/ itself is on sys.path. Without this, running the GUI
# through this CLI (as opposed to invoking server/poker.py directly, which
# implicitly puts its own directory on sys.path[0]) fails with
# "ModuleNotFoundError: No module named 'poker_conf'" the moment start_server()
# runs -- a pre-existing bug in this sys.path setup, unrelated to platform.
sys.path.append(os.path.join(src, "server"))

import click
import webbrowser

from server.poker import start_server
from config_builder import build_config

@click.group()
def cli():
    pass

@cli.command(name="serve")
@click.argument("config")
@click.option("--port", default=8000, help="port to run server")
@click.option("--speed", default="moderate", type=click.Choice(["moderate", "fast"]), help="how fast game progress")
def serve_command(config, port, speed):
    host = "localhost"
    webbrowser.open("http://%s:%s" % (host, port))
    start_server(config, port, speed)

@cli.command(name="build_config")
@click.option("-r", "--maxround", default=10, help="final round of the game")
@click.option("-s", "--stack", default=100, help="start stack of player")
@click.option("-b", "--small_blind", default=5, help="amount of small blind")
@click.option("-a", "--ante", default=0, help="amount of ante")
def build_config_command(maxround, stack, small_blind, ante):
    build_config(maxround, stack, small_blind, ante, None)


if __name__ == '__main__':
    cli()

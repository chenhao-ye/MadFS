#!/usr/bin/env python3
import logging
import re
from argparse import ArgumentParser
from pathlib import Path

import pandas as pd
from matplotlib import pyplot as plt

from plot_utils import get_sorted_subdirs, get_fs_name, export_results, plot_single_bm
from utils import get_latest_result, root_dir

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("plot_ycsb")


def parse_file(path):
    with open(path, "r") as f:
        data = f.read()
        total_num_requests = sum(
            int(e) for e in re.findall("Finished (.+?) requests", data)
        )
        total_time_us = sum(
            float(e) for e in re.findall("Time elapsed: (.+?) us", data)
        )
        mops_per_sec = total_num_requests / total_time_us
        return mops_per_sec


def parse_results(result_dir):
    results = []
    for path in get_sorted_subdirs(result_dir):
        fs_name = get_fs_name(path.name)

        for w in ("a", "b", "c", "d", "e", "f"):
            result_path = path / f"{w}-run.log"
            if not result_path.exists():
                logger.warning(f"{result_path} does not exist")
                continue
            mops_per_sec = parse_file(result_path)
            results.append(
                {
                    "x": w.upper(),
                    "y": mops_per_sec,
                    "label": fs_name,
                    "benchmark": "ycsb",
                }
            )

    df = pd.DataFrame(results)
    return df


def plot_ycsb(result_dir):
    df = parse_results(result_dir)
    export_results(result_dir, df)

    def post_plot(ax, **kwargs):
        plt.xlabel("Workload")
        plt.ylabel("Throughput (Mops/s)")
        plt.legend()

    plot_single_bm(
        df,
        barchart=True,
        figsize=(5, 2.5),
        result_dir=result_dir,
        post_plot=post_plot,
        separate_legend=False,
    )


if __name__ == "__main__":
    parser = ArgumentParser()
    parser.add_argument("--result_dir", help="Directory with results", type=Path,
                        default=get_latest_result(root_dir / "results" / f"bench_leveldb_ycsb"))
    args = parser.parse_args()
    plot_ycsb(args.result_dir)

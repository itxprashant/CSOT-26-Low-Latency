#!/usr/bin/env python3
import argparse, random
def write_graph(adj, out_path):
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(f"{len(adj)}\n")
        for nbrs in adj: f.write(" ".join([str(len(nbrs))] + [str(x) for x in nbrs]) + "\n")
def gen_grid(rows, cols):
    n = rows * cols; adj = [[] for _ in range(n)]
    def vid(r, c): return r * cols + c
    for r in range(rows):
        for c in range(cols):
            v = vid(r, c)
            if r > 0: adj[v].append(vid(r - 1, c))
            if r + 1 < rows: adj[v].append(vid(r + 1, c))
            if c > 0: adj[v].append(vid(r, c - 1))
            if c + 1 < cols: adj[v].append(vid(r, c + 1))
    return adj
def gen_er(n, deg, seed):
    rnd = random.Random(seed); adj = [[] for _ in range(n)]
    for v in range(n):
        seen = set()
        while len(seen) < deg:
            u = rnd.randrange(n)
            if u != v: seen.add(u)
        adj[v] = list(seen)
    return adj
def gen_star(n):
    adj = [[] for _ in range(n)]
    if n > 0: adj[0] = list(range(1, n))
    return adj
def gen_chain(n):
    adj = [[] for _ in range(n)]
    for i in range(n - 1): adj[i].append(i + 1)
    return adj
def main():
    p = argparse.ArgumentParser()
    p.add_argument('--kind', required=True, choices=['grid','er','star','chain'])
    p.add_argument('--out', required=True)
    p.add_argument('--rows', type=int, default=0)
    p.add_argument('--cols', type=int, default=0)
    p.add_argument('--n', type=int, default=0)
    p.add_argument('--deg', type=int, default=4)
    p.add_argument('--seed', type=int, default=1)
    a = p.parse_args()
    if a.kind == 'grid': adj = gen_grid(a.rows, a.cols)
    elif a.kind == 'er': adj = gen_er(a.n, a.deg, a.seed)
    elif a.kind == 'star': adj = gen_star(a.n)
    else: adj = gen_chain(a.n)
    write_graph(adj, a.out)
if __name__ == '__main__': main()

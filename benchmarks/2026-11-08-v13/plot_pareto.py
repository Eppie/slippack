#!/usr/bin/env python3
"""Render the joint, randomized custom-preset experiment (requires matplotlib)."""
import json
from pathlib import Path
import statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
report = json.loads((root/'pareto-mixed.json').read_text())
plt.rcParams.update({'font.size': 10, 'axes.spines.top': False,
                     'axes.spines.right': False, 'svg.fonttype': 'none'})
fig, axes = plt.subplots(1, 2, figsize=(11, 5), sharey=True)
styles = [('previous_custom', 'V12', '#737b88', '^'),
          ('custom', 'V13', '#1264a3', 'o')]
for ax, metric, label in zip(axes, ('encode', 'decode'), ('Compression', 'Decompression')):
    for codec, name, color, marker in styles:
        rows = [r for r in report['rows'] if r['codec'] == codec]
        for i, r in enumerate(rows):
            q1, _, q3 = statistics.quantiles(r['samples'][metric], n=4, method='inclusive')
            ax.errorbar(r[metric], r['bytes']/1e6,
                        xerr=[[r[metric]-q1], [q3-r[metric]]], fmt=marker,
                        color=color, markersize=6, capsize=3, linewidth=1,
                        label=name if i == 0 else None)
            level = '3–8' if codec == 'previous_custom' and r['level'] == 3 else str(r['level'])
            offset = (6, -14 if r['level'] == 9 else 10)
            if codec == 'custom' and r['level'] == 7:
                offset = (-24, -15) if metric == 'encode' else (10, 0)
            ax.annotate('L'+level, (r[metric], r['bytes']/1e6),
                        xytext=offset, textcoords='offset points', color=color, fontsize=9)
        frontier, best = [], float('inf')
        for r in sorted(rows, key=lambda x: (x[metric], x['bytes'])):
            if r['bytes'] < best:
                frontier.append(r); best = r['bytes']
        ax.plot([r[metric] for r in frontier], [r['bytes']/1e6 for r in frontier],
                color=color, alpha=.5, linewidth=1)
    ax.set_xlabel(label+' time (seconds)')
    ax.set_title(label+' tradeoff', loc='left', fontweight='bold')
    ax.grid(alpha=.18)
    ax.margins(x=.18, y=.15)
fig.legend(*axes[0].get_legend_handles_labels(), frameon=False,
           loc='upper right', bbox_to_anchor=(.98, .935), ncol=2)
axes[0].set_ylabel('Archive size (MB; lower is better)')
fig.suptitle('SLPZ custom codec — mixed replay set', fontsize=15, fontweight='bold', x=.07, ha='left')
fig.text(.07, .89, 'Lower left is better. Lines join measured nondominated points.', color='#444b55')
fig.text(.07, .035, '9 warmed rounds · 16 threads · presets randomized together · whiskers show interquartile ranges',
         fontsize=9, color='#444b55')
fig.subplots_adjust(left=.075, right=.98, bottom=.15, top=.79, wspace=.15)
fig.savefig(root/'pareto-mixed.png', dpi=160)
fig.savefig(root/'pareto-mixed.svg', metadata={'Date': None})
svg = root/'pareto-mixed.svg'
svg.write_text('\n'.join(line.rstrip() for line in svg.read_text().splitlines())+'\n')

"""Monta a página servida pelo ESP: cabeçalho HTML + index.html, comprimido com gzip.

Uso: pack.py <index.html> <saída.html.gz>
"""
import gzip
import sys

HEAD = (
    '<!doctype html><html lang="pt-BR"><head><meta charset="utf-8">'
    '<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">'
    '</head><body>\n'
)

src, dst = sys.argv[1], sys.argv[2]
with open(src, encoding='utf-8') as f:
    page = HEAD + f.read() + '\n</body></html>\n'
with open(dst, 'wb') as f:
    # mtime=0: o mesmo conteúdo sempre gera o mesmo arquivo
    f.write(gzip.compress(page.encode('utf-8'), compresslevel=9, mtime=0))

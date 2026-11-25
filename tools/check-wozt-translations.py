#!/usr/bin/env python3
"""Inventory literal GTK UI strings; dynamic expressions require manual review."""
import argparse, ast, collections, json, pathlib, re, sys
ROOT = pathlib.Path(__file__).resolve().parents[1]
LANGS = 'fr de it es pt ru ja ko zh-cn'.split()
# Argument positions are zero-based. Internal page IDs are intentionally excluded.
SINKS = {
    'gtk_label_new': (0,), 'gtk_label_new_with_mnemonic': (0,),
    'gtk_label_set_text': (1,), 'wozt_ui_label_set_text': (1,),
    'gtk_label_set_markup': (1,), 'gtk_button_new_with_label': (0,),
    'gtk_button_new_with_mnemonic': (0,), 'gtk_button_set_label': (1,),
    'wozt_ui_button_set_label': (1,), 'gtk_check_button_new_with_label': (0,),
    'gtk_menu_item_new_with_label': (0,), 'gtk_menu_item_new_with_mnemonic': (0,),
    'gtk_widget_set_tooltip_text': (1,), 'gtk_entry_set_placeholder_text': (1,),
    'gtk_combo_box_text_append_text': (1,), 'gtk_combo_box_text_append': (2,),
    'gtk_window_set_title': (1,), 'gtk_status_icon_set_tooltip_text': (1,),
    'gtk_dialog_new_with_buttons': (0,3,5,7,9), 'gtk_message_dialog_new': (4,),
    'gtk_dialog_add_button': (1,), 'gtk_color_button_set_title': (1,),
    'add_section_header': (2,), 'add_row': (2,), 'make_scale': (4,),
    'make_check': (1,), 'make_button': (1,3), 'make_stack_page': (2,3),
    'make_page': (0,), 'add_client_status': (2,), 'wozt_window_control': (0,1),
    'gtk_shell_show_error': (1,),
}
TOK = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|[^\s]')
TECH = re.compile(r'(?:[\W\d_]+|\d+[x×]\d+(?:p\d+)?|[\w.-]+/[^ ]+|(?:[A-Fa-f0-9]{2}:){5}[A-Fa-f0-9]{2})\Z')
def scan(path):
    src = path.read_text(encoding='utf-8')
    tokens = [(m.group(),m.start()) for m in TOK.finditer(src) if not m.group().startswith(('//','/*'))]
    found = collections.defaultdict(set); dynamic = set()
    for i,(name,pos) in enumerate(tokens[:-1]):
        if name not in SINKS or tokens[i+1][0] != '(': continue
        args=[[]]; depth=0
        for tok,_ in tokens[i+2:]:
            if tok==')' and depth==0: break
            if tok in ('(', '[', '{'): depth+=1
            elif tok in (')', ']', '}'): depth-=1
            if tok==',' and depth==0: args.append([])
            else: args[-1].append(tok)
        loc=f'{path.relative_to(ROOT)}:{src.count(chr(10),0,pos)+1}'
        for n in SINKS[name]:
            if n>=len(args): continue
            arg=args[n]; groups=[]; current=[]
            for tok in arg+[';']:
                if tok.startswith('"'): current.append(tok)
                elif current: groups.append(current); current=[]
            if not groups and arg and arg != ['NULL']: dynamic.add((loc,name,n))
            for group in groups:
                try: val=''.join(ast.literal_eval(t) for t in group)
                except (ValueError,SyntaxError): continue
                if val.strip() and not TECH.fullmatch(val.strip()): found[val].add(loc)
    return found,dynamic

def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--strict',action='store_true'); p.add_argument('--json',type=pathlib.Path); a=p.parse_args()
    catalogs={}
    for f in sorted((ROOT/'data/languages').glob('*.json')):
        obj=json.loads(f.read_text(encoding='utf-8')); lang=obj['id']
        if lang in catalogs: raise SystemExit(f'duplicate locale: {lang}')
        catalogs[lang]=obj['translations']
    if set(LANGS)-catalogs.keys(): raise SystemExit('missing locale files')
    found=collections.defaultdict(set); dynamic=set()
    for path in sorted((ROOT/'server').glob('*.c')):
        hits,unresolved=scan(path)
        for key,locations in hits.items(): found[key].update(locations)
        dynamic.update(unresolved)
    missing={lang:sorted(k for k in found if not isinstance(catalogs[lang].get(k),str) or not catalogs[lang][k].strip()) for lang in LANGS}
    report={'sources':{k:sorted(v) for k,v in sorted(found.items())},'missing':missing,'dynamic_review':[list(x) for x in sorted(dynamic)]}
    if a.json: a.json.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f'{len(found)} literal candidates; {len(dynamic)} dynamic arguments for review')
    for lang in LANGS:
        print(f'{lang}: {len(found)-len(missing[lang])}/{len(found)} covered; {len(missing[lang])} missing')
        for key in missing[lang]: print(f'  {key!r} ({", ".join(sorted(found[key]))})')
    if a.strict and any(missing.values()): return 1
    return 0
if __name__=='__main__': sys.exit(main())

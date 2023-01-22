" Vim syntax file
" Language: Glos

" Usage Instructions
" Put this file in .vim/syntax/glos.vim
" and add in your .vimrc file the next line:
" autocmd BufRead,BufNewFile *.glos set filetype=glos

if exists("b:current_syntax")
  finish
endif

setlocal cindent
setlocal cinkeys-=0#
setlocal cinoptions+=+0,p0
setlocal commentstring=#%s

syntax match Number "\<[0-9][0-9_]*\>"
syntax match Comment "#.*" contains=Todo
syntax match Character "'\(\\[nrt0'"\\]\|[^'\\]\)'"
syntax region String start='"' skip='\\\\\|\\"' end='"'
syntax keyword Todo TODO XXX FIXME NOTE
syntax keyword Keyword if else for match break return sizeof assert as use fn let const struct
syntax keyword Special argc argv
syntax keyword Boolean true false

let b:current_syntax = "glos"

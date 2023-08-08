if !exists("g:whisper_dir")
    let g:whisper_dir = expand($WHISPER_CPP_HOME)
    if g:whisper_dir == ""
        echoerr "Please provide a path to the whisper.cpp repo in either the $WHISPER_CPP_HOME environment variable, or g:whisper_dir"
    endif
endif
if !exists("g:whisper_lsp_path")
    let g:whisper_lsp_path = g:whisper_dir .. "lsp"
    if !filereadable(g:whisper_lsp_path)
        echoerr "Was not able to locate a lsp executable at: " .. g:whisper_lsp_path
        throw "Executable not found"
    endif
endif
if !exists("g:whisper_model_path")
    " TODO: allow paths relative the repo dir
    let g:whisper_model_path = g:whisper_dir .. "models/ggml-base.en.bin"
    if !filereadable(g:whisper_model_path)
        echoerr "Could not find model at: " .. g:whisper_model_path
        throw "Model not found"
    endif
endif
let s:output_buffer = bufnr("whisper_log", v:true)
call setbufvar(s:output_buffer,"&buftype","nofile")
let s:lsp_command = [g:whisper_lsp_path,"-m",g:whisper_model_path]
"For faster execution. TODO: server load multiple models/run multiple servers?
"let s:lsp_command = [g:whisper_lsp_path, "-m", g:whisper_dir .. "models/ggml-tiny.en.bin", "-ac", "128"]

"requestCommands([params_dict])
func whisper#requestCommands(...)
    let l:req = {"method": "guided", "params": {"commandset_index": 0}}
    if a:0 > 0
        call extend(l:req.params, a:1)
    endif
    let resp = ch_sendexpr(g:lsp_job, l:req, {"callback": function("s:commandCallback", [l:req.params, 0])})
endfunction

"doTranscription([params_dict])
func whisper#doTranscription(...)
    let l:req = {"method": "unguided", "params": {}}
    if a:0 > 0
        call extend(l:req.params, a:1)
    endif
    let resp = ch_sendexpr(g:lsp_job, l:req, {"callback": function("s:transcriptionCallback", [function("s:insertText"),function("s:endTranscription")])})
endfunction

"For testing
func whisper#uppertest(cha)
    echo tr(a:cha, s:c_lowerkeys, s:c_upperkeys)
endfunction


"(upper, exit, count, motion, command, insert/append, save run) "base"
"(upper, exit, count, motion, command, inside/around)           "motion/visual"
"(upper, exit, count, motion, line,    inside/around)           "command already entered"
"(upper, exit, key,                                 )           "from/till"

"upper and lower keys is used to translate between cases with tr
"Must be sunchronized
let s:c_lowerkeys = "1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./\""
let s:c_upperkeys = "!@#$%^&*()_+QWERTYUIOP{}|ASDFGHJKL:\"ZXCVBNM<>?'"
let s:c_count = split("1234567890",'\zs')
let s:c_command = split("ryuogpdxcv.ia", '\zs')
let s:c_motion = split("wetfhjklnb",'\zs')
"Special commands.
let s:c_special_always = ["exit", "upper"]
let s:c_special_normal = ["save", "run"]

"If not in dict, key is spoken word,
"If key resolves to string, value is used for normal/motion, but key for chars
"If key resolves to list, ["normal","motion","single char"]
let s:spoken_dict = {"w": "word", "e": "end", "r": "replace", "t": "till", "y": "yank", "u": "undo", "i": ["insert", "inside", "i"], "o": "open", "p": "paste",  "a": ["append", "around", "a"], "s": "substitute", "d": "delete", "f": "from", "g": "go", "h": "left", "j": "down", "k": "up", "l": "right", "c": "change", "v": "visual", "b": "back", "n": "next", "m": "mark", ".": ["repeat","repeat","period"], "[": ["[","[","brace"], "'": ["'",  "'", "apostrophe"], '"': ['"','"',"quotation"], "-": ["minus", "minus", "minus"]}

"Give this another pass. This seems overly hacky even if it's functional
let s:sub_tran_msg = ""
func s:subTranProg(msg)
    if s:sub_tran_msg != ""
        let s:sub_tran_msg = s:sub_tran_msg .. a:msg
        exe "normal" "u" .. s:sub_tran_msg
    else
        if s:command_backlog == ""
            " this should not occur
            call s:logCallback(0, "Warning: Encountered sub transcription without prior command")
            let s:command_backlog = "a"
        endif
        if a:msg[0] == ' '
            let s:sub_tran_msg = s:command_backlog .. a:msg[1:-1]
        else
            let s:sub_tran_msg = s:command_backlog  .. a:msg
        endif
        exe "normal" s:sub_tran_msg
    endif
    call appendbufline(s:output_buffer, "$", s:sub_tran_msg ..  ":" .. string(a:msg ))
endfunction

func s:subTranFinish(params, timestamp)
    let s:sub_tran_msg = ""
    let s:command_backlog = ""
    exe "normal a\<C-G>u"
    let l:params = a:params
    let l:params.timestamp = a:timestamp
    if exists("l:params.commandset_index")
        unlet l:params.commandset_index
    endif
    call whisper#requestCommands(a:params)
endfunction

func s:logCallback(channel, msg)
    call appendbufline(s:output_buffer,"$",a:msg)
endfunction


func s:transcriptionCallback(progressCallback, finishedCallback, channel, msg)
    let l:tr = a:msg.result.transcription

    let l:ex_ind = match(tolower(l:tr),"exit", len(l:tr)-6)
    "The worst case I've observed so far is " Exit.", which is 6 characters
    if l:ex_ind != -1
        call a:progressCallback(strpart(l:tr,0,l:ex_ind-1))
        call a:finishedCallback(a:msg.result.timestamp)
    else
        call a:progressCallback(l:tr)
        let req = {"method": "unguided", "params": {"timestamp": a:msg.result.timestamp, "no_context": v:true}}
        let resp = ch_sendexpr(g:lsp_job, req, {"callback": function("s:transcriptionCallback", [a:progressCallback, a:finishedCallback])})
    endif
endfunc
func s:insertText(msg)
    exe "normal a" .. a:msg
endfunction
func s:endTranscription(timestamp)
    call appendbufline(s:output_buffer, "$", "Ending unguided transcription")
endfunction



" If a command does not include a whole actionable step, attempting to execute
" it discards the remainder of things. There is likely a simpler solution,
" but it can be made functional now by storing a backbuffer until actionable
let s:command_backlog = ""
let s:preceeding_upper = v:false
func s:commandCallback(params, commandset_index, channel, msg)
    let l:command_index = a:msg.result.command_index
    let l:do_execute = v:false
    let l:next_mode = a:commandset_index
    let l:command = s:commandset_list[a:commandset_index][l:command_index]
    call s:logCallback(0, string(a:msg) .. " " .. a:commandset_index .. " " .. l:command)
    if l:command_index == 0
        "exit
        "if s:command_backlog == ""
        call s:logCallback(0,"Stopping command mode")
        echo "No longer listening"
        return
        "else
        " Legacy code to clear an existing buffer with exit.
        " Was found to be rarely desired and is better introduced as a
        " standalone command (clear?)
        "   call s:logCallback(0,"Clearing command_backlog" .. s:command_backlog)
        "   let s:command_backlog = ""
        "   let s:preceeding_upper = v:false
        "endif
    elseif l:command_index == 1
        "upper
        let s:preceeding_upper = !s:preceeding_upper
    elseif l:command == "save"
        " save and run can only happen in commandset 0,
        exe "w"
    elseif l:command == "run"
        exe "make run"
    else
        if s:preceeding_upper
            "Upper should keep commandset
            let s:preceeding_upper = v:false
            let l:visual_command = tr(l:command, s:c_lowerkeys, s:c_upperkeys)
        else
            let l:visual_command = l:command
        endif
        echo s:command_backlog .. " - " .. l:visual_command
        let s:command_backlog = s:command_backlog .. l:visual_command
        if a:commandset_index == 2
            "single key, either completes motion or replace
            "Should move to execute unless part of a change
            "Presence of a c anywhere but the last character is a change.
            "a c as the last character always functions as a motion (cc / fc)
            if match(s:command_backlog[0:-2], 'c') != -1
                let l:req = {"method": "unguided", "params": a:params}
                let l:req.params.timestamp = a:msg.result.timestamp
                let l:req.params.no_context = v:true
                let resp = ch_sendexpr(g:lsp_job, req, {"callback": function("s:transcriptionCallback", [function("s:subTranProg"), function("s:subTranFinish", [a:params])])})
                return
            else
                let l:do_execute = v:true
                let l:next_mode = 0
            endif
            "commandset index only matters for a/i
        elseif (l:command == "a" || l:command == "i") && a:commandset_index == 1
            "inside/around. likely deserving of it's own command set
            "For now though, single key will function
            let l:next_mode = 2
        elseif index(s:c_count, l:command) != -1
            let l:next_mode = a:commandset_index
        elseif index(s:c_motion, l:command) != -1
            if l:command == 't' || l:command == 'f'
                "prompt single key
                let l:next_mode = 2
            else
                let l:do_execute = v:true
                let l:next_mode = 0
            endif
        elseif index(s:c_command, l:command) != -1
            if index(["y","g","d","c"], s:command_backlog[-1:-1]) != -1 && s:command_backlog[-1:-1] != s:command_backlog[-2:-2] && mode() !=? 'v'
                "need motion or repeated command
                "Potential for bad state here if disparaging command keys are
                "entered (i.e. yd), but vim can handle checks for this at exe
                "And checking for cases like y123d would complicate things
                let l:next_mode = 1
            elseif index(["i","a","c", "o", "s"], l:command) != -1 || s:command_backlog[-1:-1] == 'R'
                "'Insert' mode, do general transcription
                let l:req = {"method": "unguided", "params": a:params}
                let l:req.params.timestamp = a:msg.result.timestamp
                let l:req.params.no_context = v:true
                let resp = ch_sendexpr(g:lsp_job, req, {"callback": function("s:transcriptionCallback", [function("s:subTranProg"), function("s:subTranFinish", [a:params])])})
                return
            elseif l:command == 'r'
                let l:next_mode = 2
            else
                if l:command ==? 'v'
                    let l:next_mode = 1
                else
                    let l:next_mode = 0
                endif
                let l:do_execute=v:true
            endif
        else
            throw "Invalid command state: " .. l:command .. " " .. a:commandset_index .. " " s:command_backlog
        endif
    endif
    if l:do_execute
        exe "normal" s:command_backlog
        exe "normal a\<C-G>u"
        let s:command_backlog = ""
    endif
    let l:req = {"method": "guided", "params": a:params}
    let l:req.params.timestamp = a:msg.result.timestamp
    let l:req.params.commandset_index = l:next_mode
    let resp = ch_sendexpr(g:lsp_job, l:req, {"callback": function("s:commandCallback",[a:params, l:next_mode])})
endfunction

func s:loadedCallback(channel, msg)
    echo "Loading complete"
    call s:logCallback(a:channel, a:msg)
endfunction

func s:registerCommandset(commandlist, is_final)
    let req = {"method": "registerCommandset"}
    let req.params = a:commandlist
    call s:logCallback(0, join(a:commandlist))
    if a:is_final
        let resp = ch_sendexpr(g:lsp_job, req, {"callback": "s:loadedCallback"})
    else
        let resp = ch_sendexpr(g:lsp_job, req, {"callback": "s:logCallback"})
    endif
endfunction
func s:registerAllCommands()
    let s:commandset_list = [0,0,0]
    let l:normal = s:c_special_always + s:c_special_normal + s:c_count + s:c_command + s:c_motion
    let l:visual = s:c_special_always + s:c_count + s:c_command + s:c_motion
    "Currently the same as visual.
    "let l:post_command = s:c_special_always + s:c_count + s:c_command + s:c_motion
    let l:single_key = s:c_special_always + split(s:c_lowerkeys, '\zs')
    let s:commandset_list[0] = l:normal
    call s:registerCommandset(s:commandsetToSpoken(l:normal, 0), v:false)
    let s:commandset_list[1] = l:visual
    call s:registerCommandset(s:commandsetToSpoken(l:visual, 1), v:false)
    let s:commandset_list[2] = l:single_key
    call s:registerCommandset(s:commandsetToSpoken(l:single_key, 2), v:true)
endfunction

func s:commandsetToSpoken(commandset, spoken_index)
    let l:spoken_list = []
    for l:command in a:commandset
        if has_key(s:spoken_dict, l:command)
            let l:spoken_value = s:spoken_dict[l:command]
            if type(l:spoken_value) == v:t_list
                let l:spoken_value = l:spoken_value[a:spoken_index]
            else
                if a:spoken_index == 2
                    let l:spoken_value = l:command
                endif
            endif
        else
            let l:spoken_value = l:command
        endif
        call add(l:spoken_list, l:spoken_value)
    endfor
    return l:spoken_list
endfunction

" TODO: Check lifetime. If the script is resourced, is the existing
" s:wlsp_job dropped and therefore killed?
" This seems to not be the case and I've had to deal with zombie processes
" that survive exiting vim, even though this should not be the case
"let s:lsp_opts = {"in_mode": "lsp", "out_mode": "lsp", "err_mode": "nl", "out_cb": function('LspOutCallback'), "err_cb": function("LspErrCallback"), "exit_cb": function("LspExitCallback")}
let s:lsp_opts = {"in_mode": "lsp", "out_mode": "lsp", "err_mode": "nl", "err_io": "buffer", "err_buf": s:output_buffer}
if !exists("g:lsp_job")
    let g:lsp_job = job_start(s:lsp_command, s:lsp_opts)
    if job_status(g:lsp_job) == "fail"
        echoerr "Failed to start whisper job"
    endif
    call s:registerAllCommands()
endif

SCRIPT_NAME = 'script_a'
function script_a_update(data)
    if not data.i then
        data.i = 0
    end
    data.i = data.i + 1
    locked_print(string.format('[w:%d][self:%d] i\'m script "%s", persistant i:%d',
                        data.worker_id,
                        data.instance_id,
                        SCRIPT_NAME,
                        data.i))
    if data.i > 25 then
        locked_print(string.format('script %s : %d called lua_stop()\n', SCRIPT_NAME, data.instance_id))
        lua_stop()
    end
end

widget = {
    plugin = 'xkb',
    cb = function(t)
        if t.name == 'us' then
            return '[En]'
        elseif t.name == 'ru(winkeys)' then
            return '[Ru]'
        else
            return '[??]'
        end
    end,
}

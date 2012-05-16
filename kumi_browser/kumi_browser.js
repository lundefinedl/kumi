var KUMI = (function($) {
    "use strict";
    var kumi = {};
    var wsUri = "ws://127.0.0.1:9002";
    var output;
    var smoothie;
    var websocket;
    var fps_series;
    var fps_interval_id;
    var demo_info = { effects :[] };

    var timeline_header_height = 40;
    var timeline_margin = 10;
    var ms_per_pixel = 10.0;
    var timeline_scales = [1, 5, 10, 20, 50, 100, 250, 500, 1000];
    var cur_timeline_scale = 1;
    var timeline_ofs = 0;

    var effect_height = 30;

    var is_playing = false;
    var cur_time = 0;

    var playing_id;

    var snapping_size = 50; // ms
    var snapping = true;


    var requestAnimationFrame = window.requestAnimationFrame || window.mozRequestAnimationFrame ||
        window.webkitRequestAnimationFrame || window.msRequestAnimationFrame;

    var button_state = [];

    function ptInRect(x, y, top, left, bottom, right) {
        return x >= left && x < right && y >= top && y < bottom;
    }

    function openWebSocket()
    {
        var initial_open = true;
        var retry_count = 0;
        window.console.log(wsUri);
        websocket = new WebSocket(wsUri);

        websocket.onopen = function(e) {
            // icons from http://findicons.com/pack/2103/discovery
            $('#connection-status').attr('src', 'assets/gfx/connected.png');
            websocket.send('REQ:DEMO.INFO');
            fps_interval_id = setInterval(function() { websocket.send('REQ:SYSTEM.FPS'); }, 100);
            smoothie.start();
            initial_open = false;
        };

        websocket.onclose = function(e) {
            clearInterval(fps_interval_id);
            $('#connection-status').attr('src', 'assets/gfx/disconnected.png');
            smoothie.stop();
            if (initial_open && retry_count < 10) {
                ++retry_count;
                setTimeout(openWebSocket, 2500);
            }
        };

        websocket.onmessage = function(e) {
            var res = JSON.parse(e.data);

            if (res['system.ms'] !== undefined) {
                var fps = 1000 * res['system.ms'];
                fps_series.append(new Date().getTime(), fps);
                $('#cur-fps').text(fps.toFixed(2) + ' fps');
            } else if (res.demo) {
                demo_info = res.demo;
            }
        };

        websocket.onerror = function(e) {
            window.console.log(e);
        };
    }

    kumi.onBtnPrev = function() {
        var canvas = $('#timeline-canvas');
        var delta = rawPixelToTime(canvas.width() -  timeline_margin);
        cur_time = Math.max(0, cur_time - delta);
        timeline_ofs = Math.max(0, timeline_ofs - delta);
        sendTimeInfo();
    };

    kumi.onBtnPrevSkip = function() {
        cur_time = 0;
        timeline_ofs = 0;
        sendTimeInfo();
    };


    kumi.onBtnPlay = function(playing) {
        is_playing = playing;
        if (is_playing) {
            var start_time = new Date().getTime();
            playing_id = setInterval(function() {
                var now = new Date().getTime();
                cur_time += now - start_time;
                start_time = now;
            }, 100);
        } else {
            clearInterval(playing_id);
        }
        sendTimeInfo();
    };

    kumi.onBtnNext = function() {
        var canvas = $('#timeline-canvas');
        var delta = rawPixelToTime(canvas.width() - timeline_margin);
        cur_time = cur_time + delta;
        timeline_ofs += delta;
        sendTimeInfo();
    };

    kumi.onBtnNextSkip = function() {
        sendTimeInfo();
    };

    function sendJson(type, data) {
        websocket.send(JSON.stringify( { msg : {
            type : type,
            data : data }
        }));

    }

    function sendTimeInfo() {
        sendJson("time", { is_playing : (is_playing ? true : false), cur_time : cur_time} );
    }

    function sendDemoInfo() {
        sendJson("demo", demo_info);
    }

    function setCanvasDispatch(fn) {
        var canvas = $('#timeline-canvas');
        var fns = ['mouseup', 'mousedown', 'mousemove', 'mouseleave', 'mousewheel'];
        if (setCanvasDispatch.prev_fn) {
            fns.forEach(function(f) {
                if (setCanvasDispatch.prev_fn[f]) canvas.unbind(f, setCanvasDispatch.prev_fn[f]);
            });
        }
        setCanvasDispatch.prev_fn = fn;

        fns.forEach(function(f) {
            if (fn[f]) canvas[f].apply(canvas, [fn[f]]);
        });

        if (fn.enterState)
            fn.enterState();
    }

    var insideTimeline = function(x, y, canvas) {
        // x,y are relative the document
        var ofs = canvas.offset();
        x -= ofs.left;
        y -= ofs.top;
        return ptInRect(x, y, 0, timeline_margin, timeline_header_height, canvas.width());
    };

    var insideEffect = function(x, y, canvas) {
        var ofs = canvas.offset();
        x -= ofs.left;
        y -= (ofs.top + timeline_header_height);
        var found_effect = null;
        var start;
        $.each(demo_info.effects, function(i, e) {
            if (y >= i * effect_height & y < (i+1) * effect_height) {
                var sx = timeToPixel(e.start_time) + timeline_margin;
                var ex = timeToPixel(e.end_time) + timeline_margin;
                if (Math.abs(x-sx) < 5) {
                    found_effect = e;
                    start = true;
                } else if (Math.abs(x-ex) < 5) {
                    found_effect = e;
                    start = false;
                }
            }
        });
        return { effect : found_effect, start : start };
    };

    function snap(value) {
        if (!snapping)
            return value;
        return value -= (value % snapping_size);
    }

    // Normal editor state
    var stateNormal = function(spec) {
        var canvas = $('#timeline-canvas');
        var that = {};
        var button_state = [];

        that.getCanvas = function() {
            return canvas;
        };

        that.enterState = function() {
            canvas.get(0).style.cursor = 'default';
            $('#dbg-time').text('---');
        };

        that.mouseleave = function(event) {
            var i;
            for (i = 0; i < button_state.length; ++i)
                button_state[i] = 0;
        };

        that.mousewheel = function(event, delta) {
            cur_timeline_scale = Math.min(timeline_scales.length-1, Math.max(0, cur_timeline_scale + (delta < 0 ? -1 : 1)));
            ms_per_pixel = timeline_scales[cur_timeline_scale];
        };

        that.mousemove = function(event) {
            var x = event.pageX, y = event.pageY;
            var r = insideEffect(x, y, canvas);
            if (r.effect) {
                canvas.get(0).style.cursor = 'w-resize';
            } else {
                canvas.get(0).style.cursor = 'default';
            }
        };

        that.mousedown = function(event) {
            var ofs = canvas.offset();
            var effect, r;
            var x = event.pageX, y = event.pageY;
            button_state[event.which] = 1;
            if (insideTimeline(x, y, canvas)) {
                setCanvasDispatch(stateDraggingTimeline({
                    pos : { x : event.pageX, y : event.pageY },
                    move_offset : event.shiftKey
                }));
            } else {
                r = insideEffect(x, y, canvas);
                if (r.effect && event.shiftKey )
                    setCanvasDispatch(stateDraggingEffect({
                        effect : r.effect,
                        start : r.start,
                        pos : { x : event.pageX, y : event.pageY }
                    }));
            }
        };

        return that;
    };

    var stateDraggingTimeline = function(spec) {
        var that = stateNormal(spec);
        var start_pos, org_timeline_ofs;

        that.enterState = function() {
            that.start_pos = spec.pos;
            that.move_offset = spec.move_offset;
            that.org_timeline_ofs = timeline_ofs;
        };

        var updateTime = function(pageX) {
            var ofs = that.getCanvas().offset();
            var x = pageX - ofs.left;
            cur_time = snap(Math.max(0, pixelToTime(x - timeline_margin)));
            sendTimeInfo();
        };

        that.mousemove = function(event) {
            var delta, x;
            if (that.move_offset) {
                delta = rawPixelToTime(event.pageX - that.start_pos.x);
                timeline_ofs = snap(Math.max(0, that.org_timeline_ofs - delta));
            } else {
                updateTime(event.pageX);
            }
        };

        that.mouseup = function(event) {
            button_state[event.which] = 0;
            if (!that.move_offset) {
                updateTime(event.pageX);
            }
            setCanvasDispatch(stateNormal({}));
        };

        that.mouseleave = that.mouseup;

        return that;
    };

    var stateDraggingEffect = function(spec) {
        var start_pos, effect;
        var that = stateNormal(spec);

        var canvas = $('#timeline-canvas').get(0);
        var ctx = canvas.getContext('2d');

        that.enterState = function() {
            that.effect = spec.effect;
            that.start_pos = spec.pos;
            that.dragging_start = spec.start; // are we dragging the start of end time?
            that.org_start = that.effect.start_time;
            that.org_end = that.effect.end_time;
        };

        that.mouseup = function(event) {
            sendDemoInfo();
            setCanvasDispatch(stateNormal({}));
        };

        var updateTime = function(pageX) {
            var ofs = that.getCanvas().offset();
            var x = pageX - ofs.left;
            cur_time = snap(Math.max(0, pixelToTime(x - timeline_margin)));
            sendTimeInfo();
        };

        that.mousemove = function(event) {
            var delta = rawPixelToTime(event.pageX - that.start_pos.x);
            if (that.dragging_start) {
                that.effect.start_time = snap(Math.max(0, Math.min(that.org_start + delta, that.org_end - 50)));
                $('#dbg-time').text((that.effect.start_time/1000).toFixed(2)+'s');
            } else {
                that.effect.end_time = snap(Math.max(that.org_start + 50, that.org_end + delta));
                $('#dbg-time').text((that.effect.end_time/1000).toFixed(2)+'s');
            }
            sendDemoInfo();
        };

        that.mouseleave = that.mouseup;

        return that;
    };

    function timelineInit() {
        setCanvasDispatch(stateNormal({}));

        requestAnimationFrame(drawTimeline);
    }

    function rawPixelToTime(px) {
        return px * ms_per_pixel;
    }

    function pixelToTime(px) {
        return timeline_ofs + px * ms_per_pixel;
    }

    function timeToPixel(t) {
        return (t - timeline_ofs) / ms_per_pixel;
    }

    function rawTimeToPixel(t) {
        return t / ms_per_pixel;
    }

    function drawTimelineCanvas() {
        var canvas = $('#timeline-canvas').get(0);
        var ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);

        ctx.save();

        ctx.font = "8px Arial";
        ctx.textBaseline = "top";
        ctx.textAlign = 'center';

        var h = 25 - 15;
        var grad = ctx.createLinearGradient(0, 0, 0, 40);
        grad.addColorStop(0, '#444');
        grad.addColorStop(1, '#ccc');
        ctx.fillStyle = grad;
        ctx.fillRect(0,0,canvas.width, 40);
        ctx.strokeRect(0,0,canvas.width, 40.5);

        ctx.fillStyle = '#fff';
        ctx.strokeStyle = '#888';
        for (var i = 0; i < canvas.width; i += 10) {
            var cur = pixelToTime(i);
            var extra = (i % 50) === 0;
            if (extra) {
                ctx.fillText(cur, timeline_margin + i, 15);
            }
            ctx.beginPath();
            var x = timeline_margin + i + 0.5;
            ctx.moveTo(x, 0);
            ctx.lineTo(x, 8 + (extra ? 5 : 2));
            ctx.closePath();
            ctx.stroke();
        }

        var y = timeline_header_height;
        demo_info.effects.forEach( function(e) {
            var blue_grad = ctx.createLinearGradient(0, y, 0, y+effect_height);
            blue_grad.addColorStop(0, '#558');
            blue_grad.addColorStop(1, '#aae');

            ctx.fillStyle = blue_grad;
            var o = timeline_margin + Math.max(0, timeToPixel(e.start_time));
            var w = Math.max(0,timeToPixel(e.end_time - e.start_time));
            ctx.fillRect(o, y, w, effect_height);
            y += 15;
        });

        ctx.strokeStyle = '#f33';
        ctx.beginPath();
        var m = timeline_margin + timeToPixel(cur_time);
        ctx.moveTo(m, 0);
        ctx.lineTo(m, canvas.height);
        ctx.closePath();
        ctx.stroke();

        ctx.strokeStyle = '#111';
        ctx.strokeRect(0, 0, canvas.width, canvas.height);

        ctx.restore();
    }

    function drawTimelineControl() {
        var canvas = $('#timeline-control').get(0);
        var ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);

        ctx.save();
        var d = demo_info;

        ctx.font = "15px Arial";
        ctx.textBaseline = 'middle';
        ctx.textAlign = 'center';

        var h = 25 - 15;
        var grey_grad = ctx.createLinearGradient(0, 0, 0, timeline_header_height);
        grey_grad.addColorStop(0, '#444');
        grey_grad.addColorStop(1, '#ccc');
        ctx.fillStyle = grey_grad;
        ctx.fillRect(0,0,canvas.width, 40);
        ctx.strokeRect(0,0,canvas.width, 40.5);

        var y = timeline_header_height;
        demo_info.effects.forEach( function(e) {
            var blue_grad = ctx.createLinearGradient(0, y, 0, y+effect_height);
            blue_grad.addColorStop(0, '#558');
            blue_grad.addColorStop(1, '#aae');

            ctx.fillStyle = blue_grad;
            ctx.fillRect(0, y, canvas.width, effect_height);
            ctx.fillStyle = '#000';
            ctx.fillText(e.name, canvas.width/2, y + 15);
            y += 15;
        });

        ctx.strokeStyle = '#111';
        ctx.strokeRect(0, 0, canvas.width, canvas.height);

        ctx.restore();

    }

    function drawTimeline() {

        $('#cur-time').text((cur_time/1000).toFixed(2)+'s');
        var canvas = $('#timeline-canvas').get(0);
        // scroll timeline into view
        if (timeToPixel(cur_time) > canvas.width) {
            timeline_ofs = pixelToTime(canvas.width);
        }

        drawTimelineCanvas();
        drawTimelineControl();
        requestAnimationFrame(drawTimeline);
    }

    kumi.kbInit = function() {
        output = document.getElementById("output");
        smoothie = new window.SmoothieChart();
        fps_series = new window.TimeSeries();
        smoothie.streamTo(document.getElementById("fps-canvas"), 0, false);
        smoothie.addTimeSeries(fps_series);
        openWebSocket();

        timelineInit();
    };

    return kumi;
}(window.jQuery));


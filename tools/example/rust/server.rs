use cfx::Server;
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::collections::HashMap;

fn extract_id(source: &str) -> &str {
        source.split(':').nth(1).unwrap_or(source)
}

cfx::cfx_worker! {
        fn count_primes(input: &[u8], result: &mut [u8]) -> i32 {
                let limit: u32 = std::str::from_utf8(input).ok().and_then(|s| s.parse().ok()).unwrap_or(0);
                let count = (2..limit).filter(|&n| (2..).take_while(|d| d * d <= n).all(|d| n % d != 0)).count();
                let out = count.to_string();
                let len = out.len().min(result.len());
                result[..len].copy_from_slice(&out.as_bytes()[..len]);
                len as i32
        }
}

Server! {
        cfx::trace!("Instantiated instance of script {}.\n", cfx::get_current_resource_name());

        let pending: Rc<RefCell<HashMap<String, String>>> = Rc::new(RefCell::new(HashMap::new()));
        let players: Rc<RefCell<HashMap<String, String>>> = Rc::new(RefCell::new(HashMap::new()));

        let players_stop = players.clone();
        cfx::on_stop(move || {
                cfx::trace!("Resource stopping, {} players tracked.\n", players_stop.borrow().len());
        });

        let pending_connecting = pending.clone();
        cfx::on("playerConnecting", move |source: &str, name: String| {
                let id = extract_id(source).to_string();
                pending_connecting.borrow_mut().insert(id, name);
        });

        let pending_joining = pending.clone();
        let players_joining = players.clone();
        cfx::on("playerJoining", move |source: &str, old_id: String| {
                let name = pending_joining.borrow_mut().remove(&old_id).unwrap_or_else(|| "Unknown".to_string());
                let id = extract_id(source).to_string();
                players_joining.borrow_mut().insert(id.clone(), name.clone());
                let license = cfx::natives::cfx::get_player_identifier_by_type(&id, "license2");
                let count = players_joining.borrow().len();
                cfx::trace!("{} [{}] ({}) has connected. Players Online: {}\n", name, license, id, count);
        });

        let players_dropped = players.clone();
        cfx::on("playerDropped", move |source: &str, reason: String| {
                let id = extract_id(source).to_string();
                let name = players_dropped.borrow().get(&id).cloned().unwrap_or_else(|| "Unknown".to_string());
                let license = cfx::natives::cfx::get_player_identifier_by_type(&id, "license2");
                players_dropped.borrow_mut().remove(&id);
                let count = players_dropped.borrow().len();
                cfx::trace!("{} [{}] ({}) has disconnected ({}). Players Online: {}\n", name, license, id, reason, count);
        });

        cfx::on_net("chatMessage", |_source: &str, author: i32, name: String, message: String| {
                cfx::trace!("{} ({}) has sent a chat message: {}\n", name, author, message);
        });

        cfx::add_state_bag_change_handler("", "", |bag_name, key, _value, source, replicated| {
                cfx::trace!("[cfx::add_state_bag_change_handler] {}:{} changed (source={}, replicated={})\n", bag_name, key, source, if replicated { "true" } else { "false" });
        });

        cfx::add_export("getPlayerInfo", |args| {
                if args.is_empty() {
                        return cfx::Value::from("unknown");
                }
                let src = std::ffi::CString::new(args.int(0).to_string()).unwrap_or_default();
                let name = cfx::native_string("GET_PLAYER_NAME", &[cfx::NativeArg::ptr(src.as_ptr() as u64)]);
                let ping = cfx::native_int("GET_PLAYER_PING", &[cfx::NativeArg::ptr(src.as_ptr() as u64)]);
                cfx::Value::from(format!("{} (ping={}ms)", name, ping))
        });

        cfx::set_timeout(5000, || {
                cfx::trace!("[cfx::set_timeout] 5 seconds since start.\n");
        });

        let minutes = Rc::new(Cell::new(0));
        cfx::set_interval(60000, move || {
                minutes.set(minutes.get() + 1);
                cfx::trace!("[cfx::set_interval] Up for {} minute(s).\n", minutes.get());
        });

        let worker = cfx::create_worker("count_primes", b"100000");
        if worker < 0 {
                cfx::trace!("[cfx::create_worker] Failed to start worker ({}).\n", worker);
        } else {
                let poll = Rc::new(Cell::new(0));
                let poll_id = poll.clone();
                poll.set(cfx::set_interval(100, move || {
                        let result = cfx::poll_worker(worker, 64);
                        if result.is_running() {
                                return;
                        }
                        cfx::clear_interval(poll_id.get());
                        if result.is_done() {
                                cfx::trace!("[cfx::create_worker] {} primes below 100000.\n", result.output_str());
                        } else {
                                cfx::trace!("[cfx::create_worker] Worker failed ({}).\n", result.status);
                        }
                }));
        }

        cfx::on_command("rust", |source, _args| {
                if source.is_empty() || source == "0" {
                        return;
                }
                let src: i32 = source.parse().unwrap_or(0);
                let name = cfx::get_current_resource_name();
                let info = cfx::call_export(&name, "getPlayerInfo", &[cfx::Value::from(src)]);
                cfx::trace!("[cfx::add_export] {}\n", info.as_str());
                cfx::call_export("fw", "SendChatMessage", &[cfx::Value::from(src), cfx::Value::from("^#f0a0e4[INFO] ^#ffffffHello from ^#b7410eRust^#ffffff!")]);
        }, false);
}

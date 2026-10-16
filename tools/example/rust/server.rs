use cfx::Server;
use std::cell::Cell;
use std::collections::HashMap;

static PENDING: cfx::Global<HashMap<i32, String>> = cfx::Global::new(HashMap::new);
static PLAYERS: cfx::Global<HashMap<i32, String>> = cfx::Global::new(HashMap::new);

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
        println!("Instantiated instance of script {}.", cfx::get_current_resource_name());

        cfx::on_stop(|| {
                println!("Resource stopping, {} players tracked.", PLAYERS.borrow().len());
        });

        cfx::on("playerConnecting", |source: i32, name: String| {
                PENDING.borrow_mut().insert(source, name);
        });

        cfx::on("playerJoining", |source: i32, old_id: String| {
                let name = old_id.parse().ok().and_then(|id| PENDING.borrow_mut().remove(&id)).unwrap_or_else(|| "Unknown".to_string());
                PLAYERS.borrow_mut().insert(source, name.clone());
                let license = cfx::natives::get_player_identifier_by_type(source, "license2");
                let count = PLAYERS.borrow().len();
                println!("{} [{}] ({}) has connected. Players Online: {}", name, license, source, count);
        });

        cfx::on("playerDropped", |source: i32, reason: String| {
                let name = PLAYERS.borrow_mut().remove(&source).unwrap_or_else(|| "Unknown".to_string());
                let license = cfx::natives::get_player_identifier_by_type(source, "license2");
                let count = PLAYERS.borrow().len();
                println!("{} [{}] ({}) has disconnected ({}). Players Online: {}", name, license, source, reason, count);
        });

        cfx::on_net("chatMessage", |_source: i32, author: i32, name: String, message: String| {
                println!("{} ({}) has sent a chat message: {}", name, author, message);
        });

        cfx::add_state_bag_change_handler("", "", |bag_name, key, _value, source, replicated| {
                println!("[cfx::add_state_bag_change_handler] {}:{} changed (source={}, replicated={})", bag_name, key, source, replicated);
        });

        cfx::add_export("getPlayerInfo", |source: i32| {
                let src = std::ffi::CString::new(source.to_string()).unwrap_or_default();
                let name = cfx::native_string("GET_PLAYER_NAME", &[cfx::NativeArg::ptr(src.as_ptr() as u64)]);
                let ping = cfx::native_int("GET_PLAYER_PING", &[cfx::NativeArg::ptr(src.as_ptr() as u64)]);
                format!("{} (ping={}ms)", name, ping)
        });

        cfx::set_timeout(5000, || {
                println!("[cfx::set_timeout] 5 seconds since start.");
        });

        let minutes = Cell::new(0);
        cfx::set_interval(60000, move || {
                minutes.set(minutes.get() + 1);
                println!("[cfx::set_interval] Up for {} minute(s).", minutes.get());
        });

        cfx::create_thread(async {
                let result = cfx::run_worker("count_primes", b"100000").await;
                if result.is_done() {
                        println!("[cfx::run_worker] {} primes below 100000.", result.output_str());
                } else {
                        println!("[cfx::run_worker] Worker failed ({}).", result.status);
                }
        });

        cfx::on_command("rust", |source, _args| {
                if source == 0 {
                        return;
                }
                let name = cfx::get_current_resource_name();
                let info = cfx::call_export(&name, "getPlayerInfo", &[cfx::Value::from(source)]);
                println!("[cfx::add_export] {}", info.as_str());
                cfx::call_export("fw", "SendChatMessage", &[cfx::Value::from(source), cfx::Value::from("^#f0a0e4[INFO] ^#ffffffHello from ^#b7410eRust^#ffffff!")]);
        }, false);
}

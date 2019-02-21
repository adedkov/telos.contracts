#include "../include/trail.hpp"

trail::trail(name self, name code, datastream<const char*> ds) : contract(self, code, ds) {}

trail::~trail() {}

//actions

void trail::newballot(name ballot_name, name category, name publisher, 
    string title, string description, string info_url, 
    uint8_t max_votable_options, symbol voting_sym) 
{
    require_auth(publisher);

    //check ballot doesn't already exist
    ballots ballots(get_self(), get_self().value);
    auto b = ballots.find(ballot_name.value);
    check(b == ballots.end(), "ballot name already exists");
    check(max_votable_options > 0, "max votable options must be greater than 0");

    //check category is in supported list

    vector<option> blank_options;

    ballots.emplace(publisher, [&](auto& row){
        row.ballot_name = ballot_name;
        row.category = category;
        row.publisher = publisher;
        row.title = title;
        row.description = description;
        row.info_url = info_url;
        row.options = blank_options;
        row.unique_voters = 0;
        row.max_votable_options = max_votable_options;
        row.voting_symbol = voting_sym;
        row.begin_time = 0;
        row.end_time = 0;
        row.status = SETUP;
    });

}

void trail::setinfo(name ballot_name, name publisher, string title, string description, string info_url) {
    require_auth(publisher);

    //get ballot
    ballots ballots(get_self(), get_self().value);
    auto bal = ballots.get(ballot_name.value, "ballot name doesn't exist");

    //validate
    check(bal.publisher == publisher, "only ballot publisher can set info");
    check(bal.status == SETUP, "ballot must be in setup mode to edit");

    ballots.modify(bal, same_payer, [&](auto& row) {
        row.title = title;
        row.description = description;
        row.info_url = info_url;
    });

}

void trail::addoption(name ballot_name, name publisher, name option_name, string option_info) {
    require_auth(publisher);

    //get ballot
    ballots ballots(get_self(), get_self().value);
    auto bal = ballots.get(ballot_name.value, "ballot name doesn't exist");

    //validate
    check(bal.publisher == publisher, "only ballot publisher can add options");
    check(bal.status == SETUP, "ballot must be in setup mode to edit");
    check(is_option_in_ballot(option_name, bal.options) == false, "option is already in ballot");

    option new_option = {
        option_name,
        option_info,
        asset(0, bal.voting_symbol)
    };

    ballots.modify(bal, same_payer, [&](auto& row) {
        row.options.emplace_back(new_option); //TODO: maybe push_back()?
    });
}

void trail::readyballot(name ballot_name, name publisher, uint32_t end_time) {
    require_auth(publisher);

    //get ballot
    ballots ballots(get_self(), get_self().value);
    auto bal = ballots.get(ballot_name.value, "ballot name doesn't exist");

    //validate
    check(bal.publisher == publisher, "only ballot publisher can ready ballot");
    check(bal.options.size() >= 2, "ballot must have at least 2 options");
    check(bal.status == SETUP, "ballot must be in setup mode to edit");
    check(end_time - now() >= MIN_BALLOT_LENGTH, "ballot must be open for at least 1 day");

    ballots.modify(bal, same_payer, [&](auto& row) {
        row.begin_time = now();
        row.end_time = end_time;
        row.status = OPEN;
    });

}

void trail::closeballot(name ballot_name, name publisher, uint8_t new_status) {
    require_auth(publisher);

    //get ballot
    ballots ballots(get_self(), get_self().value);
    auto bal = ballots.get(ballot_name.value, "ballot name doesn't exist");

    //validate
    check(bal.publisher == publisher, "only ballot publisher can ready ballot");
    check(bal.status == OPEN, "ballot must be in open mode to close");
    check(bal.end_time < now(), "must be past ballot end time to close");

    ballots.modify(bal, same_payer, [&](auto& row) {
        row.status = CLOSED;
    });
}

void trail::deleteballot(name ballot_name, name publisher) {
    require_auth(publisher);

    //get ballot
    ballots ballots(get_self(), get_self().value);
    auto bal = ballots.get(ballot_name.value, "ballot name doesn't exist");

    //validate
    check(bal.publisher == publisher, "only ballot publisher can ready ballot");
    check(bal.status != OPEN, "cannot delete while voting is in progress");

    //TODO: add exception for polls 

    ballots.erase(bal);
}

void trail::vote(name voter, name ballot_name, name option) {
    require_auth(voter);

    //TODO: attempt to clean up at least 2 old votes

    //get ballot
    ballots ballots(get_self(), get_self().value);
    auto bal = ballots.get(ballot_name.value, "ballot name doesn't exist");
    check(now() >= bal.begin_time && now() <= bal.end_time, "must vote between ballot's begin and end time");
    check(bal.status == OPEN, "ballot status is not open for voting");

    //get account
    accounts accounts(get_self(), voter.value);
    auto acc = accounts.get(bal.voting_symbol.code().raw(), "account balance not found");
    check(acc.num_votes < MAX_VOTE_RECEIPTS, "reached max concurrent votes for voting token");
    check(acc.balance.amount > int64_t(0), "cannot vote with a balance of 0");

    //check option exists
    int idx = get_option_index(option, bal.options);
    check(idx != -1, "option not found on ballot");

    //get votes
    votes votes(get_self(), voter.value);
    auto v_itr = votes.find(ballot_name.value);

    if (v_itr != votes.end()) { //vote for ballot exists
        
        //validate
        check(!is_option_in_receipt(option, v_itr->option_names), "voter has already voted for this option");
        check(v_itr->option_names.size() + 1 <= bal.max_votable_options, "already voted for max number of options allowed by ballot");

        //add votes to ballot option
        ballots.modify(bal, same_payer, [&](auto& row) {
            row.options[idx].votes += acc.balance;
        });

        //update vote with new option name
        votes.modify(v_itr, same_payer, [&](auto& row) {
            row.option_names.emplace_back(option);
        });

    } else { //vote doesn't already exist

        //validate


        vector<name> new_option_names = {option};

        //emplace new vote
        votes.emplace(voter, [&](auto& row) {
            row.ballot_name = ballot_name;
            row.option_names = new_option_names;
            row.amount = acc.balance;
            row.expiration = bal.end_time;
        });
        
        //add votes to ballot option
        ballots.modify(bal, same_payer, [&](auto& row) {
            row.options[idx].votes += acc.balance;
            row.unique_voters += 1;
        });

        //update num_votes on account
        accounts.modify(acc, same_payer, [&](auto& row) {
            row.num_votes += 1;
        });

    }
}

void trail::unvote(name voter, name ballot_name, name option) {
    require_auth(voter);

    //get ballot
    ballots ballots(get_self(), get_self().value);
    auto bal = ballots.get(ballot_name.value, "ballot name doesn't exist");

    //get votes
    votes votes(get_self(), voter.value);
    auto v = votes.get(ballot_name.value, "vote does not exist for this ballot");

    //get account
    accounts accounts(get_self(), voter.value);
    auto acc = accounts.get(bal.voting_symbol.code().raw(), "account balance not found");

    auto bal_opt_idx = get_option_index(option, bal.options);
    auto new_voted_options = v.option_names;
    bool found = false;

    for (auto opt_itr = new_voted_options.begin(); opt_itr < new_voted_options.end(); opt_itr++) {
        if (*opt_itr == option) {
            new_voted_options.erase(opt_itr);
            found = true;
            break;
        }
    }

    //validate
    check(bal.status == OPEN, "ballot status is not open for voting");
    check(now() >= bal.begin_time && now() <= bal.end_time, "must unvote between ballot's begin and end time");
    check(found, "option not found on vote");
    check(bal_opt_idx != -1, "option not found on ballot");

    if (new_voted_options.size() > 0) { //votes for ballot still remain

        auto new_bal_options = bal.options;
        new_bal_options[bal_opt_idx].votes -= v.amount;
        
        //remove option from vote
        votes.modify(v, same_payer, [&](auto& row) {
            row.option_names = new_voted_options;
        });

        //lower option votes by amount
        ballots.modify(bal, same_payer, [&](auto& row) {
            row.options = new_bal_options;
        });

    } else { //unvoted last option

        //erase vote
        votes.erase(v);

        //decrement bal.unique_voters;
        ballots.modify(bal, same_payer, [&](auto& row) {
            row.unique_voters -= 1;
        });

        //decrement num_votes on account
        accounts.modify(acc, same_payer, [&](auto& row) {
            row.num_votes -= 1;
        });

    }

}

void trail::cleanupvotes(name voter, uint16_t count, symbol voting_sym) {
    
    //sort votes by expiration, lowest first
    votes votes(get_self(), voter.value);
    auto sorted_votes = votes.get_index<name("byexp")>();
    auto sv_itr = sorted_votes.begin(); //TODO: use lower_bound()?

    //deletes expired votes, skips active votes
    while (count > 0 || sv_itr != sorted_votes.end()) {
        if (sv_itr->expiration > now()) { //expired
            sv_itr = sorted_votes.erase(sv_itr); //returns next iterator
            count--;
        } else { //active
            sv_itr++;
        }
    }

}



void trail::newtoken(name publisher, asset max_supply, token_settings settings, string info_url) {
    require_auth(publisher);

    symbol new_sym = max_supply.symbol;

    //check registry doesn't already exist
    registries registries(get_self(), get_self().value);
    auto reg = registries.find(new_sym.code().raw());
    check(reg == registries.end(), "registry with symbol not found");
    check(new_sym.code().raw() != symbol("TLOS", 4).code().raw(), "the TLOS symbol is restricted to avoid confusion with the system token");

    registries.emplace(publisher, [&](auto& row){
        row.supply = asset(0, new_sym);
        row.max_supply = max_supply;
        row.publisher = publisher;
        row.total_voters = uint32_t(0);
        row.total_proxies = uint32_t(0);
        row.settings = settings;
        row.info_url = info_url;
    });

}

void trail::mint(name publisher, name recipient, asset amount_to_mint) {
    require_auth(publisher);
    check(is_account(recipient), "recipient account doesn't exist");

    symbol token_sym = amount_to_mint.symbol;

    //get registry
    registries registries(get_self(), get_self().value);
    auto reg = registries.get(token_sym.code().raw(), "registry with symbol not found");

    //get account
    accounts accounts(get_self(), recipient.value);
    auto acc = accounts.get(token_sym.code().raw(), "account balance not found");

    //validate
    check(reg.publisher == publisher, "only registry publisher can mint new tokens");
    check(reg.supply + amount_to_mint <= reg.max_supply, "cannot mint tokens beyond max_supply");
    check(amount_to_mint > asset(0, token_sym), "must mint a positive amount");
    check(amount_to_mint.is_valid(), "invalid amount");

    //update recipient balance
    accounts.modify(acc, same_payer, [&](auto& row) {
        row.balance += amount_to_mint;
    });

    //update registry supply
    registries.modify(reg, same_payer, [&](auto& row) {
        row.supply += amount_to_mint;
    });

}

void trail::burn(name publisher, asset amount_to_burn) {
    require_auth(publisher);

    symbol token_sym = amount_to_burn.symbol;

    //get registry
    registries registries(get_self(), get_self().value);
    auto reg = registries.get(token_sym.code().raw(), "registry with symbol not found");

    //get account
    accounts accounts(get_self(), publisher.value);
    auto acc = accounts.get(token_sym.code().raw(), "account balance not found");

    //validate
    check(reg.publisher == publisher, "only registry publisher can burn tokens");
    check(reg.supply - amount_to_burn >= asset(0, token_sym), "cannot burn more tokens than exist");
    check(acc.balance >= amount_to_burn, "cannot burn more tokens than owned");
    check(amount_to_burn > asset(0, token_sym), "must burn a positive amount");
    check(amount_to_burn.is_valid(), "invalid amount");

    //update publisher balance
    accounts.modify(acc, same_payer, [&](auto& row) {
        row.balance -= amount_to_burn;
    });
}

void trail::send(name sender, name recipient, asset amount, string memo) {
    require_auth(sender);

    //get registry
    registries registries(get_self(), get_self().value);
    auto reg = registries.get(amount.symbol.code().raw(), "registry with symbol not found");

    //validate
    check(sender != recipient, "cannot send tokens to yourself");
    check(is_account(recipient), "recipient account doesn't exist");
    check(amount.is_valid(), "invalid amount");
    check(amount.amount > 0, "must transfer positive amount");
    check(amount.symbol == reg.max_supply.symbol, "symbol precision mismatch");
    check(memo.size() <= 256, "memo has more than 256 bytes");

    //require_recipient(sender);
    //require_recipient(recipient);

    //sub amount from sender
    //add amount to recipient

}

void trail::seize(name publisher, name owner, asset amount_to_seize) {
    require_auth(publisher);

    symbol token_sym = amount_to_seize.symbol;

    //get registry
    registries registries(get_self(), get_self().value);
    auto reg = registries.get(token_sym.code().raw(), "registry with symbol not found");

    //validate
    check(publisher != owner, "cannot seize tokens from yourself");
    check(is_account(owner), "owner account doesn't exist");
    check(amount_to_seize.is_valid(), "invalid amount");
    check(amount_to_seize.amount > 0, "must seize positive amount");
    check(amount_to_seize.symbol == reg.max_supply.symbol, "symbol precision mismatch");

    //sub amount from owner
    //add amount to publisher

}

void trail::open(name owner, symbol token_sym) {
    require_auth(owner);

    //get registry
    registries registries(get_self(), get_self().value);
    auto reg = registries.get(token_sym.code().raw(), "registry with symbol not found");

    //check account balance doesn't already exist
    accounts accounts(get_self(), owner.value);
    auto acc = accounts.find(token_sym.code().raw());
    check(acc == accounts.end(), "account balance already exists");

    //emplace account with zero balance
    accounts.emplace(owner, [&](auto& row){
        row.balance = asset(0, token_sym);
    });
}

void trail::close(name owner, symbol token_sym) {
    require_auth(owner);

    //get account
    accounts accounts(get_self(), owner.value);
    auto acc = accounts.get(token_sym.code().raw(), "account balance doesn't exist");

    //validate
    check(acc.balance == asset(0, token_sym), "cannot close an account still holding tokens");

    accounts.erase(acc);
}


//functions

bool trail::is_option_in_ballot(name option_name, vector<option> options) {
    for (option opt : options) {
        if (option_name == opt.option_name) {
            return true;
        }
    }

    return false;
}

bool trail::is_option_in_receipt(name option_name, vector<name> options_voted) {
    return std::find(options_voted.begin(), options_voted.end(), option_name) != options_voted.end();

    // for (name n : options_voted) {
    //     if (option_name == n) {
    //         return true;
    //     }
    // }
    // return false;
}

int trail::get_option_index(name option_name, vector<option> options) {
    for (int i = 0; i < options.size(); i++) {
        if (option_name == options[i].option_name) {
            return i;
        }
    }

    return -1;
}

bool trail::has_token_balance(name voter, symbol sym) {
    accounts accounts(get_self(), voter.value);
    auto a_itr = accounts.find(sym.code().raw());
    if (a_itr != accounts.end()) {
        return true;
    }
    return false;
}

void trail::update_votes(name voter) {
    //return if no VOTE balance found, user must call open() first
    if (!has_token_balance(voter, VOTE_SYM)) {
        return;
    }

    //calc balance delta

    //revote for all active VOTE ballots (only inserting the delta)

    //update VOTE balance
}

extern "C"
{
    void apply(uint64_t receiver, uint64_t code, uint64_t action)
    {
        size_t size = action_data_size();
        constexpr size_t max_stack_buffer_size = 512;
        void* buffer = nullptr;
        if( size > 0 ) {
            buffer = max_stack_buffer_size < size ? malloc(size) : alloca(size);
            read_action_data(buffer, size);
        }
        datastream<const char*> ds((char*)buffer, size);

        if (code == receiver)
        {
            switch (action)
            {
                EOSIO_DISPATCH_HELPER(trail, (newballot)(setinfo)(addoption)(readyballot)(closeballot)(deleteballot)
                    (vote)(unvote)(cleanupvotes)(newtoken)(mint)(burn)(send)(seize)(open)(close));
            }

        } else if (code == name("eosio").value && action == name("undelegatebw").value) {

            struct undelegatebw_args {
                name from;
                name receiver;
                asset unstake_net_quantity;
                asset unstake_cpu_quantity;
            };
            
            trail trailservice(name(receiver), name(code), ds);
            auto args = unpack_action_data<undelegatebw_args>();
            trailservice.update_votes(args.from);
            //execute_action<trailservice>(eosio::name(receiver), eosio::name(code), &trailservice::update_votes());
        }
    }
}
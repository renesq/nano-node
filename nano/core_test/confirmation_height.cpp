#include <nano/core_test/testutil.hpp>
#include <nano/node/testing.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace
{
void add_callback_stats (nano::node & node)
{
	node.observers.blocks.add ([& stats = node.stats](nano::election_status const & status_a, nano::account const &, nano::amount const &, bool) {
		stats.inc (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out);
	});
}
}

TEST (confirmation_height, single)
{
	auto amount (std::numeric_limits<nano::uint128_t>::max ());
	nano::system system (24000, 2);
	nano::keypair key1;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest1 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	system.wallet (1)->insert_adhoc (key1.prv);
	auto send1 (std::make_shared<nano::send_block> (latest1, key1.pub, amount - system.nodes[0]->config.receive_minimum.number (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest1)));

	// Check confirmation heights before, should be uninitialized (1 for genesis).
	uint64_t confirmation_height;
	for (auto & node : system.nodes)
	{
		add_callback_stats (*node);
		auto transaction = node->store.tx_begin_read ();
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (1, confirmation_height);
	}

	for (auto & node : system.nodes)
	{
		node->process_active (send1);
		node->block_processor.flush ();

		system.deadline_set (10s);
		while (true)
		{
			auto transaction = node->store.tx_begin_read ();
			if (node->ledger.block_confirmed (transaction, send1->hash ()))
			{
				break;
			}

			ASSERT_NO_ERROR (system.poll ());
		}

		auto transaction = node->store.tx_begin_write ();
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (2, confirmation_height);

		// Rollbacks should fail as these blocks have been cemented
		ASSERT_TRUE (node->ledger.rollback (transaction, latest1));
		ASSERT_TRUE (node->ledger.rollback (transaction, send1->hash ()));
		ASSERT_EQ (1, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
		ASSERT_EQ (1, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	}
}

TEST (confirmation_height, multiple_accounts)
{
	nano::system system;
	nano::node_config node_config (24001, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	system.add_node (node_config);
	node_config.peering_port = 24002;
	system.add_node (node_config);
	nano::keypair key1;
	nano::keypair key2;
	nano::keypair key3;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest1 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	system.wallet (1)->insert_adhoc (key1.prv);
	system.wallet (0)->insert_adhoc (key2.prv);
	system.wallet (1)->insert_adhoc (key3.prv);

	// Send to all accounts
	nano::send_block send1 (latest1, key1.pub, system.nodes.front ()->config.online_weight_minimum.number () + 300, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest1));
	nano::send_block send2 (send1.hash (), key2.pub, system.nodes.front ()->config.online_weight_minimum.number () + 200, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send1.hash ()));
	nano::send_block send3 (send2.hash (), key3.pub, system.nodes.front ()->config.online_weight_minimum.number () + 100, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send2.hash ()));

	// Open all accounts
	nano::open_block open1 (send1.hash (), nano::genesis_account, key1.pub, key1.prv, key1.pub, *system.work.generate (key1.pub));
	nano::open_block open2 (send2.hash (), nano::genesis_account, key2.pub, key2.prv, key2.pub, *system.work.generate (key2.pub));
	nano::open_block open3 (send3.hash (), nano::genesis_account, key3.pub, key3.prv, key3.pub, *system.work.generate (key3.pub));

	// Send and recieve various blocks to these accounts
	nano::send_block send4 (open1.hash (), key2.pub, 50, key1.prv, key1.pub, *system.work.generate (open1.hash ()));
	nano::send_block send5 (send4.hash (), key2.pub, 10, key1.prv, key1.pub, *system.work.generate (send4.hash ()));

	nano::receive_block receive1 (open2.hash (), send4.hash (), key2.prv, key2.pub, *system.work.generate (open2.hash ()));
	nano::send_block send6 (receive1.hash (), key3.pub, 10, key2.prv, key2.pub, *system.work.generate (receive1.hash ()));
	nano::receive_block receive2 (send6.hash (), send5.hash (), key2.prv, key2.pub, *system.work.generate (send6.hash ()));

	for (auto & node : system.nodes)
	{
		add_callback_stats (*node);

		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send3).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open3).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send4).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send5).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send6).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive2).code);

		// Check confirmation heights of all the accounts are uninitialized (0),
		// as we have any just added them to the ledger and not processed any live transactions yet.
		uint64_t confirmation_height;
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (1, confirmation_height);
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height));
		ASSERT_EQ (0, confirmation_height);
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key2.pub, confirmation_height));
		ASSERT_EQ (0, confirmation_height);
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key3.pub, confirmation_height));
		ASSERT_EQ (0, confirmation_height);
	}

	// The nodes process a live receive which propagates across to all accounts
	auto receive3 = std::make_shared<nano::receive_block> (open3.hash (), send6.hash (), key3.prv, key3.pub, *system.work.generate (open3.hash ()));

	for (auto & node : system.nodes)
	{
		node->process_active (receive3);
		node->block_processor.flush ();

		system.deadline_set (10s);
		while (true)
		{
			auto transaction = node->store.tx_begin_read ();
			if (node->ledger.block_confirmed (transaction, receive3->hash ()))
			{
				break;
			}

			ASSERT_NO_ERROR (system.poll ());
		}

		nano::account_info account_info;
		uint64_t confirmation_height;
		auto & store = node->store;
		auto transaction = node->store.tx_begin_read ();
		ASSERT_FALSE (store.account_get (transaction, nano::test_genesis_key.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (4, confirmation_height);
		ASSERT_EQ (4, account_info.block_count);
		ASSERT_FALSE (store.account_get (transaction, key1.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height));
		ASSERT_EQ (2, confirmation_height);
		ASSERT_EQ (3, account_info.block_count);
		ASSERT_FALSE (store.account_get (transaction, key2.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key2.pub, confirmation_height));
		ASSERT_EQ (3, confirmation_height);
		ASSERT_EQ (4, account_info.block_count);
		ASSERT_FALSE (store.account_get (transaction, key3.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key3.pub, confirmation_height));
		ASSERT_EQ (2, confirmation_height);
		ASSERT_EQ (2, account_info.block_count);

		// The accounts for key1 and key2 have 1 more block in the chain than is confirmed.
		// So this can be rolled back, but the one before that cannot. Check that this is the case
		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_FALSE (node->ledger.rollback (transaction, node->latest (key2.pub)));
			ASSERT_FALSE (node->ledger.rollback (transaction, node->latest (key1.pub)));
		}
		{
			// These rollbacks should fail
			auto transaction = node->store.tx_begin_write ();
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (key1.pub)));
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (key2.pub)));

			// Confirm the other latest can't be rolled back either
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (key3.pub)));
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (nano::test_genesis_key.pub)));

			// Attempt some others which have been cemented
			ASSERT_TRUE (node->ledger.rollback (transaction, open1.hash ()));
			ASSERT_TRUE (node->ledger.rollback (transaction, send2.hash ()));
		}
		ASSERT_EQ (10, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
		ASSERT_EQ (10, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	}
}

TEST (confirmation_height, gap_bootstrap)
{
	nano::system system (24000, 1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	nano::keypair destination;
	auto send1 (std::make_shared<nano::state_block> (nano::genesis_account, genesis.hash (), nano::genesis_account, nano::genesis_amount - nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	auto send2 (std::make_shared<nano::state_block> (nano::genesis_account, send1->hash (), nano::genesis_account, nano::genesis_amount - 2 * nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*send2);
	auto send3 (std::make_shared<nano::state_block> (nano::genesis_account, send2->hash (), nano::genesis_account, nano::genesis_amount - 3 * nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*send3);
	auto open1 (std::make_shared<nano::open_block> (send1->hash (), destination.pub, destination.pub, destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*open1);

	// Receive
	auto receive1 (std::make_shared<nano::receive_block> (open1->hash (), send2->hash (), destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*receive1);
	auto receive2 (std::make_shared<nano::receive_block> (receive1->hash (), send3->hash (), destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*receive2);

	node1.block_processor.add (send1);
	node1.block_processor.add (send2);
	node1.block_processor.add (send3);
	node1.block_processor.add (receive1);
	node1.block_processor.flush ();

	add_callback_stats (node1);

	// Receive 2 comes in on the live network, however the chain has not been finished so it gets added to unchecked
	node1.process_active (receive2);
	node1.block_processor.flush ();

	// Confirmation heights should not be updated
	{
		auto transaction (node1.store.tx_begin_read ());
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 2);

		uint64_t confirmation_height;
		ASSERT_FALSE (node1.store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (1, confirmation_height);
	}

	// Now complete the chain where the block comes in on the bootstrap network.
	node1.block_processor.add (open1);
	node1.block_processor.flush ();

	// Confirmation height should be unchanged and unchecked should now be 0
	{
		auto transaction (node1.store.tx_begin_read ());
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 0);

		uint64_t confirmation_height;
		ASSERT_FALSE (node1.store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (1, confirmation_height);
		ASSERT_FALSE (node1.store.confirmation_height_get (transaction, destination.pub, confirmation_height));
		ASSERT_EQ (0, confirmation_height);
	}
	ASSERT_EQ (0, node1.stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (0, node1.stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
}

TEST (confirmation_height, gap_live)
{
	nano::system system;
	nano::node_config node_config (24001, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	system.add_node (node_config);
	node_config.peering_port = 24002;
	system.add_node (node_config);
	nano::keypair destination;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	system.wallet (1)->insert_adhoc (destination.prv);

	nano::genesis genesis;
	auto send1 (std::make_shared<nano::state_block> (nano::genesis_account, genesis.hash (), nano::genesis_account, nano::genesis_amount - nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	system.nodes[0]->work_generate_blocking (*send1);
	auto send2 (std::make_shared<nano::state_block> (nano::genesis_account, send1->hash (), nano::genesis_account, nano::genesis_amount - 2 * nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	system.nodes[0]->work_generate_blocking (*send2);
	auto send3 (std::make_shared<nano::state_block> (nano::genesis_account, send2->hash (), nano::genesis_account, nano::genesis_amount - 3 * nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	system.nodes[0]->work_generate_blocking (*send3);

	auto open1 (std::make_shared<nano::open_block> (send1->hash (), destination.pub, destination.pub, destination.prv, destination.pub, 0));
	system.nodes[0]->work_generate_blocking (*open1);
	auto receive1 (std::make_shared<nano::receive_block> (open1->hash (), send2->hash (), destination.prv, destination.pub, 0));
	system.nodes[0]->work_generate_blocking (*receive1);
	auto receive2 (std::make_shared<nano::receive_block> (receive1->hash (), send3->hash (), destination.prv, destination.pub, 0));
	system.nodes[0]->work_generate_blocking (*receive2);

	for (auto & node : system.nodes)
	{
		node->block_processor.add (send1);
		node->block_processor.add (send2);
		node->block_processor.add (send3);
		node->block_processor.add (receive1);
		node->block_processor.flush ();

		add_callback_stats (*node);

		// Receive 2 comes in on the live network, however the chain has not been finished so it gets added to unchecked
		node->process_active (receive2);
		node->block_processor.flush ();

		// Confirmation heights should not be updated
		{
			auto transaction = node->store.tx_begin_read ();
			uint64_t confirmation_height;
			ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
			ASSERT_EQ (1, confirmation_height);
		}

		// Now complete the chain where the block comes in on the live network
		node->process_active (open1);
		node->block_processor.flush ();

		system.deadline_set (10s);
		while (true)
		{
			auto transaction = node->store.tx_begin_read ();
			if (node->ledger.block_confirmed (transaction, receive2->hash ()))
			{
				break;
			}

			ASSERT_NO_ERROR (system.poll ());
		}

		// This should confirm the open block and the source of the receive blocks
		auto transaction (node->store.tx_begin_read ());
		auto unchecked_count (node->store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 0);

		uint64_t confirmation_height;
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (4, confirmation_height);
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, destination.pub, confirmation_height));
		ASSERT_EQ (3, confirmation_height);

		ASSERT_EQ (6, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
		ASSERT_EQ (6, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	}
}

TEST (confirmation_height, send_receive_between_2_accounts)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);
	nano::keypair key1;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));
	system.wallet (0)->insert_adhoc (key1.prv);

	nano::send_block send1 (latest, key1.pub, node->config.online_weight_minimum.number () + 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest));
	nano::open_block open1 (send1.hash (), nano::genesis_account, key1.pub, key1.prv, key1.pub, *system.work.generate (key1.pub));

	nano::send_block send2 (open1.hash (), nano::genesis_account, 1000, key1.prv, key1.pub, *system.work.generate (open1.hash ()));
	nano::send_block send3 (send2.hash (), nano::genesis_account, 900, key1.prv, key1.pub, *system.work.generate (send2.hash ()));
	nano::send_block send4 (send3.hash (), nano::genesis_account, 500, key1.prv, key1.pub, *system.work.generate (send3.hash ()));

	nano::receive_block receive1 (send1.hash (), send2.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send1.hash ()));
	nano::receive_block receive2 (receive1.hash (), send3.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (receive1.hash ()));
	nano::receive_block receive3 (receive2.hash (), send4.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (receive2.hash ()));

	nano::send_block send5 (receive3.hash (), key1.pub, node->config.online_weight_minimum.number () + 1, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (receive3.hash ()));
	auto receive4 = std::make_shared<nano::receive_block> (send4.hash (), send5.hash (), key1.prv, key1.pub, *system.work.generate (send4.hash ()));
	// Unpocketed send
	nano::keypair key2;
	nano::send_block send6 (send5.hash (), key2.pub, node->config.online_weight_minimum.number (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send5.hash ()));
	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open1).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive1).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send3).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send4).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive3).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send5).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send6).code);
	}

	add_callback_stats (*node);

	node->process_active (receive4);
	node->block_processor.flush ();

	system.deadline_set (10s);
	while (true)
	{
		auto transaction = node->store.tx_begin_read ();
		if (node->ledger.block_confirmed (transaction, receive4->hash ()))
		{
			break;
		}

		ASSERT_NO_ERROR (system.poll ());
	}

	auto transaction (node->store.tx_begin_read ());

	nano::account_info account_info;
	uint64_t confirmation_height;
	ASSERT_FALSE (node->store.account_get (transaction, nano::test_genesis_key.pub, account_info));
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
	ASSERT_EQ (6, confirmation_height);
	ASSERT_EQ (7, account_info.block_count);

	ASSERT_FALSE (node->store.account_get (transaction, key1.pub, account_info));
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height));
	ASSERT_EQ (5, confirmation_height);
	ASSERT_EQ (5, account_info.block_count);

	ASSERT_EQ (10, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (10, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	ASSERT_EQ (11, node->ledger.cemented_count);
}

TEST (confirmation_height, send_receive_self)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));

	nano::send_block send1 (latest, nano::test_genesis_key.pub, nano::genesis_amount - 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest));
	nano::receive_block receive1 (send1.hash (), send1.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send1.hash ()));
	nano::send_block send2 (receive1.hash (), nano::test_genesis_key.pub, nano::genesis_amount - 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (receive1.hash ()));
	nano::send_block send3 (send2.hash (), nano::test_genesis_key.pub, nano::genesis_amount - 3, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send2.hash ()));

	nano::receive_block receive2 (send3.hash (), send2.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send3.hash ()));
	auto receive3 = std::make_shared<nano::receive_block> (receive2.hash (), send3.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (receive2.hash ()));

	// Send to another account to prevent automatic receiving on the genesis account
	nano::keypair key1;
	nano::send_block send4 (receive3->hash (), key1.pub, node->config.online_weight_minimum.number (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (receive3->hash ()));
	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send3).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *receive3).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send4).code);
	}

	add_callback_stats (*node);

	node->block_confirm (receive3);

	system.deadline_set (10s);
	while (true)
	{
		auto transaction = node->store.tx_begin_read ();
		if (node->ledger.block_confirmed (transaction, receive3->hash ()))
		{
			break;
		}

		ASSERT_NO_ERROR (system.poll ());
	}

	auto transaction (node->store.tx_begin_read ());
	nano::account_info account_info;
	ASSERT_FALSE (node->store.account_get (transaction, nano::test_genesis_key.pub, account_info));
	uint64_t confirmation_height;
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
	ASSERT_EQ (7, confirmation_height);
	ASSERT_EQ (8, account_info.block_count);
	ASSERT_EQ (6, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (6, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	ASSERT_EQ (confirmation_height, node->ledger.cemented_count);
}

TEST (confirmation_height, all_block_types)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));
	nano::keypair key1;
	nano::keypair key2;
	auto & store = node->store;
	nano::send_block send (latest, key1.pub, nano::genesis_amount - nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest));
	nano::send_block send1 (send.hash (), key2.pub, nano::genesis_amount - nano::Gxrb_ratio * 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send.hash ()));

	nano::open_block open (send.hash (), nano::test_genesis_key.pub, key1.pub, key1.prv, key1.pub, *system.work.generate (key1.pub));
	nano::state_block state_open (key2.pub, 0, 0, nano::Gxrb_ratio, send1.hash (), key2.prv, key2.pub, *system.work.generate (key2.pub));

	nano::send_block send2 (open.hash (), key2.pub, 0, key1.prv, key1.pub, *system.work.generate (open.hash ()));
	nano::state_block state_receive (key2.pub, state_open.hash (), 0, nano::Gxrb_ratio * 2, send2.hash (), key2.prv, key2.pub, *system.work.generate (state_open.hash ()));

	nano::state_block state_send (key2.pub, state_receive.hash (), 0, nano::Gxrb_ratio, key1.pub, key2.prv, key2.pub, *system.work.generate (state_receive.hash ()));
	nano::receive_block receive (send2.hash (), state_send.hash (), key1.prv, key1.pub, *system.work.generate (send2.hash ()));

	nano::change_block change (receive.hash (), key2.pub, key1.prv, key1.pub, *system.work.generate (receive.hash ()));

	nano::state_block state_change (key2.pub, state_send.hash (), nano::test_genesis_key.pub, nano::Gxrb_ratio, 0, key2.prv, key2.pub, *system.work.generate (state_send.hash ()));

	nano::state_block epoch (key2.pub, state_change.hash (), nano::test_genesis_key.pub, nano::Gxrb_ratio, node->ledger.epoch_link (nano::epoch::epoch_1), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (state_change.hash ()));

	nano::state_block epoch1 (key1.pub, change.hash (), key2.pub, nano::Gxrb_ratio, node->ledger.epoch_link (nano::epoch::epoch_1), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (change.hash ()));
	nano::state_block state_send1 (key1.pub, epoch1.hash (), 0, nano::Gxrb_ratio - 1, key2.pub, key1.prv, key1.pub, *system.work.generate (epoch1.hash ()));
	nano::state_block state_receive2 (key2.pub, epoch.hash (), 0, nano::Gxrb_ratio + 1, state_send1.hash (), key2.prv, key2.pub, *system.work.generate (epoch.hash ()));

	auto state_send2 = std::make_shared<nano::state_block> (key2.pub, state_receive2.hash (), 0, nano::Gxrb_ratio, key1.pub, key2.prv, key2.pub, *system.work.generate (state_receive2.hash ()));
	nano::state_block state_send3 (key2.pub, state_send2->hash (), 0, nano::Gxrb_ratio - 1, key1.pub, key2.prv, key2.pub, *system.work.generate (state_send2->hash ()));

	nano::state_block state_send4 (key1.pub, state_send1.hash (), 0, nano::Gxrb_ratio - 2, nano::test_genesis_key.pub, key1.prv, key1.pub, *system.work.generate (state_send1.hash ()));
	nano::state_block state_receive3 (nano::genesis_account, send1.hash (), nano::genesis_account, nano::genesis_amount - nano::Gxrb_ratio * 2 + 1, state_send4.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send1.hash ()));

	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_open).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_receive).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_send).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, change).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_change).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, epoch).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, epoch1).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_receive2).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *state_send2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_send3).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_send4).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_receive3).code);
	}

	add_callback_stats (*node);
	node->block_confirm (state_send2);

	system.deadline_set (10s);
	while (true)
	{
		auto transaction = node->store.tx_begin_read ();
		if (node->ledger.block_confirmed (transaction, state_send2->hash ()))
		{
			break;
		}

		ASSERT_NO_ERROR (system.poll ());
	}

	auto transaction (node->store.tx_begin_read ());
	nano::account_info account_info;
	uint64_t confirmation_height;
	ASSERT_FALSE (node->store.account_get (transaction, nano::test_genesis_key.pub, account_info));
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
	ASSERT_EQ (3, confirmation_height);
	ASSERT_LE (4, account_info.block_count);

	ASSERT_FALSE (node->store.account_get (transaction, key1.pub, account_info));
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height));
	ASSERT_EQ (6, confirmation_height);
	ASSERT_LE (7, account_info.block_count);

	ASSERT_FALSE (node->store.account_get (transaction, key2.pub, account_info));
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, key2.pub, confirmation_height));
	ASSERT_EQ (7, confirmation_height);
	ASSERT_LE (8, account_info.block_count);

	ASSERT_EQ (15, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (15, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	ASSERT_EQ (16, node->ledger.cemented_count);
}

/* Bulk of the this test was taken from the node.fork_flip test */
TEST (confirmation_height, conflict_rollback_cemented)
{
	boost::iostreams::stream_buffer<nano::stringstream_mt_sink> sb;
	sb.open (nano::stringstream_mt_sink{});
	nano::boost_log_cerr_redirect redirect_cerr (&sb);
	nano::system system (24000, 2);
	auto & node1 (*system.nodes[0]);
	auto & node2 (*system.nodes[1]);
	ASSERT_EQ (1, node1.network.size ());
	nano::keypair key1;
	nano::genesis genesis;
	auto send1 (std::make_shared<nano::send_block> (genesis.hash (), key1.pub, nano::genesis_amount - 100, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	nano::publish publish1 (send1);
	nano::keypair key2;
	auto send2 (std::make_shared<nano::send_block> (genesis.hash (), key2.pub, nano::genesis_amount - 100, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (genesis.hash ())));
	nano::publish publish2 (send2);
	auto channel1 (node1.network.udp_channels.create (node1.network.endpoint ()));
	node1.network.process_message (publish1, channel1);
	node1.block_processor.flush ();
	auto channel2 (node2.network.udp_channels.create (node1.network.endpoint ()));
	node2.network.process_message (publish2, channel2);
	node2.block_processor.flush ();
	ASSERT_EQ (1, node1.active.size ());
	ASSERT_EQ (1, node2.active.size ());
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	node1.network.process_message (publish2, channel1);
	node1.block_processor.flush ();
	node2.network.process_message (publish1, channel2);
	node2.block_processor.flush ();
	nano::unique_lock<std::mutex> lock (node2.active.mutex);
	auto conflict (node2.active.roots.find (nano::qualified_root (genesis.hash (), genesis.hash ())));
	ASSERT_NE (node2.active.roots.end (), conflict);
	auto votes1 (conflict->election);
	ASSERT_NE (nullptr, votes1);
	ASSERT_EQ (1, votes1->last_votes.size ());
	lock.unlock ();
	// Force blocks to be cemented on both nodes
	{
		auto transaction (system.nodes[0]->store.tx_begin_write ());
		ASSERT_TRUE (node1.store.block_exists (transaction, publish1.block->hash ()));
		node1.store.confirmation_height_put (transaction, nano::genesis_account, 2);
	}
	{
		auto transaction (system.nodes[1]->store.tx_begin_write ());
		ASSERT_TRUE (node2.store.block_exists (transaction, publish2.block->hash ()));
		node2.store.confirmation_height_put (transaction, nano::genesis_account, 2);
	}

	auto rollback_log_entry = boost::str (boost::format ("Failed to roll back %1%") % send2->hash ().to_string ());
	system.deadline_set (20s);
	auto done (false);
	while (!done)
	{
		ASSERT_NO_ERROR (system.poll ());
		done = (sb.component ()->str ().find (rollback_log_entry) != std::string::npos);
	}
	auto transaction1 (system.nodes[0]->store.tx_begin_read ());
	auto transaction2 (system.nodes[1]->store.tx_begin_read ());
	lock.lock ();
	auto winner (*votes1->tally ().begin ());
	ASSERT_EQ (*publish1.block, *winner.second);
	ASSERT_EQ (nano::genesis_amount - 100, winner.first);
	ASSERT_TRUE (node1.store.block_exists (transaction1, publish1.block->hash ()));
	ASSERT_TRUE (node2.store.block_exists (transaction2, publish2.block->hash ()));
	ASSERT_FALSE (node2.store.block_exists (transaction2, publish1.block->hash ()));
}

TEST (confirmation_height, observers)
{
	auto amount (std::numeric_limits<nano::uint128_t>::max ());
	nano::system system (24000, 1);
	auto node1 (system.nodes[0]);
	nano::keypair key1;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest1 (node1->latest (nano::test_genesis_key.pub));
	auto send1 (std::make_shared<nano::send_block> (latest1, key1.pub, amount - node1->config.receive_minimum.number (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest1)));

	add_callback_stats (*node1);

	node1->process_active (send1);
	node1->block_processor.flush ();
	bool confirmed (false);
	system.deadline_set (10s);
	while (!confirmed)
	{
		auto transaction = node1->store.tx_begin_read ();
		confirmed = node1->ledger.block_confirmed (transaction, send1->hash ());
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, node1->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (1, node1->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
}

// This tests when a read has been done and the block no longer exists by the time a write is done
TEST (confirmation_height, modified_chain)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);

	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));

	nano::keypair key1;
	auto & store = node->store;
	auto send = std::make_shared<nano::send_block> (latest, key1.pub, nano::genesis_amount - nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest));

	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *send).code);
	}

	node->confirmation_height_processor.add (send->hash ());

	{
		// The write guard prevents the confirmation height processor doing any writes
		auto write_guard = node->write_database_queue.wait (nano::writer::testing);
		while (!node->write_database_queue.contains (nano::writer::confirmation_height))
			;

		store.block_del (store.tx_begin_write (), send->hash ());
	}

	while (node->write_database_queue.contains (nano::writer::confirmation_height))
		;

	ASSERT_EQ (1, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::invalid_block, nano::stat::dir::in));
}

namespace nano
{
TEST (confirmation_height, pending_observer_callbacks)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);

	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));

	nano::keypair key1;
	nano::send_block send (latest, key1.pub, nano::genesis_amount - nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest));
	auto send1 = std::make_shared<nano::send_block> (send.hash (), key1.pub, nano::genesis_amount - nano::Gxrb_ratio * 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send.hash ()));

	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *send1).code);
	}

	add_callback_stats (*node);

	node->confirmation_height_processor.add (send1->hash ());

	while (true)
	{
		if (node->pending_confirmation_height.size () == 0)
		{
			break;
		}
	}
	// Can have timing issues.
	node->confirmation_height_processor.add (send.hash ());
	{
		nano::unique_lock<std::mutex> lk (node->pending_confirmation_height.mutex);
		while (!node->pending_confirmation_height.current_hash.is_zero ())
		{
			lk.unlock ();
			std::this_thread::yield ();
			lk.lock ();
		}
	}

	// Confirm the callback is not called under this circumstance
	ASSERT_EQ (2, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (0, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	ASSERT_EQ (3, node->ledger.cemented_count);
}

TEST (confirmation_height, prioritize_frontiers)
{
	nano::system system;
	// Prevent frontiers being confirmed as it will affect the priorization checking
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);

	nano::keypair key1;
	nano::keypair key2;
	nano::keypair key3;
	nano::keypair key4;
	nano::block_hash latest1 (system.nodes[0]->latest (nano::test_genesis_key.pub));

	// Send different numbers of blocks all accounts
	nano::send_block send1 (latest1, key1.pub, node->config.online_weight_minimum.number () + 10000, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest1));
	nano::send_block send2 (send1.hash (), key1.pub, node->config.online_weight_minimum.number () + 8500, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send1.hash ()));
	nano::send_block send3 (send2.hash (), key1.pub, node->config.online_weight_minimum.number () + 8000, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send2.hash ()));
	nano::send_block send4 (send3.hash (), key2.pub, node->config.online_weight_minimum.number () + 7500, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send3.hash ()));
	nano::send_block send5 (send4.hash (), key3.pub, node->config.online_weight_minimum.number () + 6500, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send4.hash ()));
	nano::send_block send6 (send5.hash (), key4.pub, node->config.online_weight_minimum.number () + 6000, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send5.hash ()));

	// Open all accounts and add other sends to get different uncemented counts (as well as some which are the same)
	nano::open_block open1 (send1.hash (), nano::genesis_account, key1.pub, key1.prv, key1.pub, *system.work.generate (key1.pub));
	nano::send_block send7 (open1.hash (), nano::test_genesis_key.pub, 500, key1.prv, key1.pub, *system.work.generate (open1.hash ()));

	nano::open_block open2 (send4.hash (), nano::genesis_account, key2.pub, key2.prv, key2.pub, *system.work.generate (key2.pub));

	nano::open_block open3 (send5.hash (), nano::genesis_account, key3.pub, key3.prv, key3.pub, *system.work.generate (key3.pub));
	nano::send_block send8 (open3.hash (), nano::test_genesis_key.pub, 500, key3.prv, key3.pub, *system.work.generate (open3.hash ()));
	nano::send_block send9 (send8.hash (), nano::test_genesis_key.pub, 200, key3.prv, key3.pub, *system.work.generate (send8.hash ()));

	nano::open_block open4 (send6.hash (), nano::genesis_account, key4.pub, key4.prv, key4.pub, *system.work.generate (key4.pub));
	nano::send_block send10 (open4.hash (), nano::test_genesis_key.pub, 500, key4.prv, key4.pub, *system.work.generate (open4.hash ()));
	nano::send_block send11 (send10.hash (), nano::test_genesis_key.pub, 200, key4.prv, key4.pub, *system.work.generate (send10.hash ()));

	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send3).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send4).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send5).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send6).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send7).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open2).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open3).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send8).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send9).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open4).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send10).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send11).code);
	}

	auto transaction = node->store.tx_begin_read ();
	constexpr auto num_accounts = 5;
	// clang-format off
	auto priority_orders_match = [](auto const & cementable_frontiers, auto const & desired_order) {
		return std::equal (desired_order.begin (), desired_order.end (), cementable_frontiers.template get<1> ().begin (), cementable_frontiers.template get<1> ().end (), [](nano::account const & account, nano::cementable_account const & cementable_account) {
			return (account == cementable_account.account);
		});
	};
	// clang-format on
	{
		node->active.prioritize_frontiers_for_confirmation (transaction, std::chrono::seconds (1), std::chrono::seconds (1));
		ASSERT_EQ (node->active.priority_cementable_frontiers_size (), num_accounts);
		// Check the order of accounts is as expected (greatest number of uncemented blocks at the front). key3 and key4 have the same value, the order is unspecified so check both
		std::array<nano::account, num_accounts> desired_order_1{ nano::genesis_account, key3.pub, key4.pub, key1.pub, key2.pub };
		std::array<nano::account, num_accounts> desired_order_2{ nano::genesis_account, key4.pub, key3.pub, key1.pub, key2.pub };
		ASSERT_TRUE (priority_orders_match (node->active.priority_cementable_frontiers, desired_order_1) || priority_orders_match (node->active.priority_cementable_frontiers, desired_order_2));
	}

	{
		// Add some to the local node wallets and check ordering of both containers
		system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
		system.wallet (0)->insert_adhoc (key1.prv);
		system.wallet (0)->insert_adhoc (key2.prv);
		node->active.prioritize_frontiers_for_confirmation (transaction, std::chrono::seconds (1), std::chrono::seconds (1));
		ASSERT_EQ (node->active.priority_cementable_frontiers_size (), num_accounts - 3);
		ASSERT_EQ (node->active.priority_wallet_cementable_frontiers_size (), num_accounts - 2);
		std::array<nano::account, 3> local_desired_order{ nano::genesis_account, key1.pub, key2.pub };
		ASSERT_TRUE (priority_orders_match (node->active.priority_wallet_cementable_frontiers, local_desired_order));
		std::array<nano::account, 2> desired_order_1{ key3.pub, key4.pub };
		std::array<nano::account, 2> desired_order_2{ key4.pub, key3.pub };
		ASSERT_TRUE (priority_orders_match (node->active.priority_cementable_frontiers, desired_order_1) || priority_orders_match (node->active.priority_cementable_frontiers, desired_order_2));
	}

	{
		// Add the remainder of accounts to node wallets and check size/ordering is correct
		system.wallet (0)->insert_adhoc (key3.prv);
		system.wallet (0)->insert_adhoc (key4.prv);
		node->active.prioritize_frontiers_for_confirmation (transaction, std::chrono::seconds (1), std::chrono::seconds (1));
		ASSERT_EQ (node->active.priority_cementable_frontiers_size (), 0);
		ASSERT_EQ (node->active.priority_wallet_cementable_frontiers_size (), num_accounts);
		std::array<nano::account, num_accounts> desired_order_1{ nano::genesis_account, key3.pub, key4.pub, key1.pub, key2.pub };
		std::array<nano::account, num_accounts> desired_order_2{ nano::genesis_account, key4.pub, key3.pub, key1.pub, key2.pub };
		ASSERT_TRUE (priority_orders_match (node->active.priority_wallet_cementable_frontiers, desired_order_1) || priority_orders_match (node->active.priority_wallet_cementable_frontiers, desired_order_2));
	}

	// Check that accounts which already exist have their order modified when the uncemented count changes.
	nano::send_block send12 (send9.hash (), nano::test_genesis_key.pub, 100, key3.prv, key3.pub, *system.work.generate (send9.hash ()));
	nano::send_block send13 (send12.hash (), nano::test_genesis_key.pub, 90, key3.prv, key3.pub, *system.work.generate (send12.hash ()));
	nano::send_block send14 (send13.hash (), nano::test_genesis_key.pub, 80, key3.prv, key3.pub, *system.work.generate (send13.hash ()));
	nano::send_block send15 (send14.hash (), nano::test_genesis_key.pub, 70, key3.prv, key3.pub, *system.work.generate (send14.hash ()));
	nano::send_block send16 (send15.hash (), nano::test_genesis_key.pub, 60, key3.prv, key3.pub, *system.work.generate (send15.hash ()));
	nano::send_block send17 (send16.hash (), nano::test_genesis_key.pub, 50, key3.prv, key3.pub, *system.work.generate (send16.hash ()));
	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send12).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send13).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send14).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send15).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send16).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send17).code);
	}
	transaction.refresh ();
	node->active.prioritize_frontiers_for_confirmation (transaction, std::chrono::seconds (1), std::chrono::seconds (1));
	ASSERT_TRUE (priority_orders_match (node->active.priority_wallet_cementable_frontiers, std::array<nano::account, num_accounts>{ key3.pub, nano::genesis_account, key4.pub, key1.pub, key2.pub }));
	node->active.search_frontiers (transaction);

	// Check that the active transactions roots contains the frontiers
	system.deadline_set (std::chrono::seconds (10));
	while (node->active.size () != num_accounts)
	{
		ASSERT_NO_ERROR (system.poll ());
	}

	std::array<nano::qualified_root, num_accounts> frontiers{ send17.qualified_root (), send6.qualified_root (), send7.qualified_root (), open2.qualified_root (), send11.qualified_root () };
	for (auto & frontier : frontiers)
	{
		ASSERT_NE (node->active.roots.find (frontier), node->active.roots.end ());
	}
}
}

TEST (confirmation_height, frontiers_confirmation_mode)
{
	nano::genesis genesis;
	nano::keypair key;
	// Always mode
	{
		nano::system system;
		nano::node_config node_config (24000, system.logging);
		node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::always;
		auto node = system.add_node (node_config);
		nano::state_block send (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - nano::Gxrb_ratio, key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *node->work_generate_blocking (genesis.hash ()));
		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send).code);
		}
		system.deadline_set (5s);
		while (node->active.size () != 1)
		{
			ASSERT_NO_ERROR (system.poll ());
		}
	}
	// Auto mode
	{
		nano::system system;
		nano::node_config node_config (24000, system.logging);
		node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::automatic;
		auto node = system.add_node (node_config);
		nano::state_block send (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - nano::Gxrb_ratio, key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *node->work_generate_blocking (genesis.hash ()));
		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send).code);
		}
		system.deadline_set (5s);
		while (node->active.size () != 1)
		{
			ASSERT_NO_ERROR (system.poll ());
		}
	}
	// Disabled mode
	{
		nano::system system;
		nano::node_config node_config (24000, system.logging);
		node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
		auto node = system.add_node (node_config);
		nano::state_block send (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - nano::Gxrb_ratio, key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *node->work_generate_blocking (genesis.hash ()));
		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send).code);
		}
		system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
		std::this_thread::sleep_for (std::chrono::seconds (1));
		ASSERT_EQ (0, node->active.size ());
	}
}

// The callback and confirmation history should only be updated after confirmation height is set (and not just after voting)
TEST (confirmation_height, callback_confirmed_history)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);

	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));

	nano::keypair key1;
	auto & store = node->store;
	auto send = std::make_shared<nano::send_block> (latest, key1.pub, nano::genesis_amount - nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest));
	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *send).code);
	}

	auto send1 = std::make_shared<nano::send_block> (send->hash (), key1.pub, nano::genesis_amount - nano::Gxrb_ratio * 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send->hash ()));

	add_callback_stats (*node);

	node->process_active (send1);
	node->block_processor.flush ();

	{
		node->process_active (send);
		node->block_processor.flush ();
		// The write guard prevents the confirmation height processor doing any writes
		auto write_guard = node->write_database_queue.wait (nano::writer::testing);
		system.deadline_set (10s);
		while (node->active.size () > 0)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		ASSERT_EQ (0, node->active.list_confirmed ().size ());
		{
			nano::lock_guard<std::mutex> guard (node->active.mutex);
			ASSERT_EQ (0, node->active.blocks.size ());
		}

		auto transaction = node->store.tx_begin_read ();
		ASSERT_FALSE (node->ledger.block_confirmed (transaction, send->hash ()));

		system.deadline_set (10s);
		while (!node->write_database_queue.contains (nano::writer::confirmation_height))
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		// Confirm that no inactive callbacks have been called when the confirmation height processor has already iterated over it, waiting to write
		ASSERT_EQ (0, node->stats.count (nano::stat::type::observer, nano::stat::detail::observer_confirmation_inactive, nano::stat::dir::out));
	}

	system.deadline_set (10s);
	while (node->write_database_queue.contains (nano::writer::confirmation_height))
	{
		ASSERT_NO_ERROR (system.poll ());
	}

	auto transaction = node->store.tx_begin_read ();
	ASSERT_TRUE (node->ledger.block_confirmed (transaction, send->hash ()));

	ASSERT_EQ (1, node->active.list_confirmed ().size ());
	ASSERT_EQ (0, node->active.blocks.size ());

	// Confirm the callback is not called under this circumstance
	ASSERT_EQ (2, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (2, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	ASSERT_EQ (1, node->stats.count (nano::stat::type::observer, nano::stat::detail::observer_confirmation_active_quorum, nano::stat::dir::out));
	ASSERT_EQ (1, node->stats.count (nano::stat::type::observer, nano::stat::detail::observer_confirmation_inactive, nano::stat::dir::out));

	nano::lock_guard<std::mutex> guard (node->active.mutex);
	ASSERT_EQ (0, node->active.pending_conf_height.size ());
}

namespace nano
{
TEST (confirmation_height, dependent_election)
{
	nano::system system;
	nano::node_config node_config (24001, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);

	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));

	nano::keypair key1;
	auto & store = node->store;
	auto send = std::make_shared<nano::send_block> (latest, key1.pub, nano::genesis_amount - nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest));
	auto send1 = std::make_shared<nano::send_block> (send->hash (), key1.pub, nano::genesis_amount - nano::Gxrb_ratio * 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send->hash ()));
	auto send2 = std::make_shared<nano::send_block> (send1->hash (), key1.pub, nano::genesis_amount - nano::Gxrb_ratio * 3, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send1->hash ()));
	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *send).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *send2).code);
	}

	add_callback_stats (*node);

	// Prevent the confirmation height processor from doing any processing
	node->confirmation_height_processor.pause ();

	// Wait until it has been processed
	node->block_confirm (send2);
	system.deadline_set (10s);
	while (node->active.size () > 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}

	system.deadline_set (10s);
	while (node->pending_confirmation_height.size () != 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}

	{
		nano::lock_guard<std::mutex> guard (node->pending_confirmation_height.mutex);
		ASSERT_EQ (*node->pending_confirmation_height.pending.begin (), send2->hash ());
	}

	// Now put the other block in active so it can be confirmed as a dependent election
	node->block_confirm (send1);
	node->confirmation_height_processor.unpause ();

	system.deadline_set (10s);
	while (node->stats.count (nano::stat::type::observer, nano::stat::detail::observer_confirmation_active_quorum, nano::stat::dir::out) != 1 && node->stats.count (nano::stat::type::observer, nano::stat::detail::observer_confirmation_active_conf_height, nano::stat::dir::out) != 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}

	ASSERT_EQ (3, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	ASSERT_EQ (1, node->stats.count (nano::stat::type::observer, nano::stat::detail::observer_confirmation_inactive, nano::stat::dir::out));
	ASSERT_EQ (3, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));

	nano::lock_guard<std::mutex> guard (node->active.mutex);
	ASSERT_EQ (0, node->active.pending_conf_height.size ());
}

TEST (confirmation_height, dependent_election_after_already_cemented)
{
	nano::system system;
	nano::node_config node_config (24001, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);

	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));

	nano::keypair key1;
	auto & store = node->store;
	auto send = std::make_shared<nano::send_block> (latest, key1.pub, nano::genesis_amount - nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest));
	auto send1 = std::make_shared<nano::send_block> (send->hash (), key1.pub, nano::genesis_amount - nano::Gxrb_ratio * 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (send->hash ()));

	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *send).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *send1).code);
	}

	add_callback_stats (*node);

	{
		node->block_confirm (send1);
		auto write_guard = node->write_database_queue.wait (nano::writer::testing);
		system.deadline_set (10s);
		while (node->active.size () > 0)
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		ASSERT_EQ (0, node->active.list_confirmed ().size ());
		{
			nano::lock_guard<std::mutex> guard (node->active.mutex);
			ASSERT_EQ (0, node->active.blocks.size ());
		}

		auto transaction = node->store.tx_begin_read ();
		ASSERT_FALSE (node->ledger.block_confirmed (transaction, send->hash ()));

		system.deadline_set (10s);
		while (!node->write_database_queue.contains (nano::writer::confirmation_height))
		{
			ASSERT_NO_ERROR (system.poll ());
		}

		node->block_confirm (send);
		system.deadline_set (10s);
		while (node->active.size () > 0)
		{
			ASSERT_NO_ERROR (system.poll ());
		}
	}

	system.deadline_set (10s);
	while (node->pending_confirmation_height.size () != 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}

	system.deadline_set (10s);
	nano::unique_lock<std::mutex> lk (node->active.mutex);
	while (node->active.pending_conf_height.size () > 0)
	{
		lk.unlock ();
		ASSERT_NO_ERROR (system.poll ());
		lk.lock ();
	}
}
}

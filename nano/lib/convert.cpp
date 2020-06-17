#include <nano/lib/convert.hpp>

std::string convert_raw_to_dec(std::string amount_raw, nano::uint128_t ratio)
{
	std::string amount_in_dec = amount_raw;
	int amount_length = amount_raw.length ();
	int raw_length = ratio.convert_to<std::string> ().length ();

	if (raw_length > amount_length)
	{
		int rest = raw_length - amount_length;
		for (int i = 1; i < rest; i++)
		{
			amount_in_dec = "0" + amount_in_dec;
		}
		amount_in_dec = "0." + amount_in_dec;
	}
	else
	{
		amount_in_dec.insert(amount_in_dec.end() - (raw_length - 1), '.');
	}

	int i = 0;
	while(amount_in_dec[amount_in_dec.length () - 1 - i] == '0') {
		i++;
	}
	// 29 zeros
	if (i == (raw_length - 1)) {
		i--;
	}

	amount_in_dec.erase(amount_in_dec.length () - i, amount_in_dec.length ());
	return amount_in_dec;
}
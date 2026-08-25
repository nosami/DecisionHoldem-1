//###############################################################################
//
//   Copyright 2022 The DecisionHoldem Authors，namely，Qibin Zhou，
//   Dongdong Bai，Junge Zhang and Kaiqi Huang. All Rights Reserved.
//
//   Licensed under the GNU AFFERO GENERAL PUBLIC LICENSE
//                 Version 3, 19 November 2007
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
//   GNU Affero General Public License for more details.
//
//   You should have received a copy of the GNU Affero General Public License 
//   along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//###############################################################################
#include <random>
#include "Multi_Blureprint.h"

int main(int argc, char **argv) {
	// NOTE: the original code asserted argc == 1 and then tested argv[0] == 0,
	// which is always false for a normally-invoked process (argv[0] is the
	// program path), so multiprocess_blueprint() was unreachable and every
	// invocation silently fell through to the evaluation branch below. This
	// contradicted the documented usage ("./Main.o 0" trains, "./Main.o 1"
	// evaluates). Fixed to actually branch on the CLI argument as documented
	// in README.md.
	assert(argc == 2);
	if (argv[1][0] == '0')
		multiprocess_blueprint();
	else
	{
		Player players[] = { Player(20000),Player(20000) };
		PokerTable table(2, players);
		Pokerstate state(table);
		state.reset_game();
		strategy_node* root = new strategy_node();
		load(root, "cluster/blueprint_strategy.dat");
		state.reset_game(); 
		check_subgame(root, state);
		state.reset_game();
		cout << getcfv_whole_holdem(root, state, 0) << endl;
		state.reset_game();
		cout << getcfv_whole_holdem(root, state, 1) << endl;
	}
}

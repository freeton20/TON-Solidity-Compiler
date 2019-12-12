/*
 * Copyright 2018-2019 TON DEV SOLUTIONS LTD.
 *
 * Licensed under the  terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the  GNU General Public License for more details at: https://www.gnu.org/licenses/gpl-3.0.html
 */
/**
 * @author TON Labs <connect@tonlabs.io>
 * @date 2019
 * TVM constants and runtime error codes
 */

#pragma once

namespace TvmConst {
	namespace C4 {
		// length of key in dict c4
		const int KeyLength = 64;

		const int PersistenceMembersStartIndex = 100;
	}
	namespace SENDRAWMSG {
		const int CarryAllMoney  = 128;
	}
	namespace Message {
		const int functionIdLength = 32;
		const int timestampLength = 64;
		namespace ReplayProtection {
			const int Interval = 30 * 60 * 1000; // 30 min = 30 * 60 * 1000 milisec;
		}
		namespace Exception {
			const int ConstructorIsCalledTwice  = 123;
			const int ReplayProtection  = 124;
		}
	}
	const int CellBitLength = 1023;
	const int ArrayKeyLength = 32;
}
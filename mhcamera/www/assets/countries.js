const COUNTRY_CALLING_CODES = Object.freeze([
  ["CN", "中国大陆", "China mainland", "+86"],
  ["HK", "中国香港", "Hong Kong", "+852"],
  ["MO", "中国澳门", "Macao", "+853"],
  ["TW", "中国台湾", "Taiwan", "+886"],
  ["SG", "新加坡", "Singapore", "+65"],
  ["MY", "马来西亚", "Malaysia", "+60"],
  ["JP", "日本", "Japan", "+81"],
  ["KR", "韩国", "South Korea", "+82"],
  ["TH", "泰国", "Thailand", "+66"],
  ["VN", "越南", "Vietnam", "+84"],
  ["ID", "印度尼西亚", "Indonesia", "+62"],
  ["PH", "菲律宾", "Philippines", "+63"],
  ["IN", "印度", "India", "+91"],
  ["PK", "巴基斯坦", "Pakistan", "+92"],
  ["BD", "孟加拉国", "Bangladesh", "+880"],
  ["LK", "斯里兰卡", "Sri Lanka", "+94"],
  ["NP", "尼泊尔", "Nepal", "+977"],
  ["AE", "阿联酋", "United Arab Emirates", "+971"],
  ["SA", "沙特阿拉伯", "Saudi Arabia", "+966"],
  ["IL", "以色列", "Israel", "+972"],
  ["TR", "土耳其", "Türkiye", "+90"],
  ["US", "美国", "United States", "+1"],
  ["CA", "加拿大", "Canada", "+1"],
  ["MX", "墨西哥", "Mexico", "+52"],
  ["BR", "巴西", "Brazil", "+55"],
  ["AR", "阿根廷", "Argentina", "+54"],
  ["CL", "智利", "Chile", "+56"],
  ["CO", "哥伦比亚", "Colombia", "+57"],
  ["PE", "秘鲁", "Peru", "+51"],
  ["GB", "英国", "United Kingdom", "+44"],
  ["DE", "德国", "Germany", "+49"],
  ["FR", "法国", "France", "+33"],
  ["IT", "意大利", "Italy", "+39"],
  ["ES", "西班牙", "Spain", "+34"],
  ["NL", "荷兰", "Netherlands", "+31"],
  ["BE", "比利时", "Belgium", "+32"],
  ["CH", "瑞士", "Switzerland", "+41"],
  ["AT", "奥地利", "Austria", "+43"],
  ["DK", "丹麦", "Denmark", "+45"],
  ["SE", "瑞典", "Sweden", "+46"],
  ["NO", "挪威", "Norway", "+47"],
  ["FI", "芬兰", "Finland", "+358"],
  ["PL", "波兰", "Poland", "+48"],
  ["CZ", "捷克", "Czechia", "+420"],
  ["RU", "俄罗斯", "Russia", "+7"],
  ["UA", "乌克兰", "Ukraine", "+380"],
  ["AU", "澳大利亚", "Australia", "+61"],
  ["NZ", "新西兰", "New Zealand", "+64"],
  ["ZA", "南非", "South Africa", "+27"],
  ["EG", "埃及", "Egypt", "+20"],
  ["NG", "尼日利亚", "Nigeria", "+234"],
  ["KE", "肯尼亚", "Kenya", "+254"]
].map(([iso2, name_zh, name_en, calling_code]) => Object.freeze({
  iso2,
  name_zh,
  name_en,
  calling_code
})));

const DEFAULT_COUNTRY_KEY = "CN:+86";

class PhoneInputError extends Error {
  constructor(code) {
    super(code);
    this.name = "PhoneInputError";
    this.code = code;
  }
}

class SmsCodeError extends Error {
  constructor() {
    super("invalid_sms_code");
    this.name = "SmsCodeError";
    this.code = "invalid_sms_code";
  }
}

function countryKey(country) {
  return `${country.iso2}:${country.calling_code}`;
}

function callingCodeForCountryKey(key) {
  const country = COUNTRY_CALLING_CODES.find((item) => countryKey(item) === key);
  return country ? country.calling_code : "";
}

function normalizePhoneFields(callingCode, nationalNumber) {
  const normalizedCallingCode = String(callingCode || "").trim();
  const normalizedNationalNumber = String(nationalNumber || "").trim();
  if (!/^\+[1-9][0-9]{0,2}$/.test(normalizedCallingCode)) {
    throw new PhoneInputError("invalid_calling_code");
  }
  if (!/^[0-9]+$/.test(normalizedNationalNumber)) {
    throw new PhoneInputError("invalid_national_number");
  }
  if (normalizedCallingCode.length - 1 + normalizedNationalNumber.length > 15) {
    throw new PhoneInputError("phone_number_too_long");
  }
  return Object.freeze({
    calling_code: normalizedCallingCode,
    national_number: normalizedNationalNumber
  });
}

function normalizeSmsCode(value, expectedLength) {
  const code = String(value || "").trim();
  if (
    !Number.isSafeInteger(expectedLength) ||
    expectedLength <= 0 ||
    code.length !== expectedLength ||
    !/^[0-9]+$/.test(code)
  ) {
    throw new SmsCodeError();
  }
  return code;
}

export {
  COUNTRY_CALLING_CODES,
  DEFAULT_COUNTRY_KEY,
  PhoneInputError,
  SmsCodeError,
  callingCodeForCountryKey,
  countryKey,
  normalizePhoneFields,
  normalizeSmsCode
};

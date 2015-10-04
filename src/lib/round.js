export default {
  roundPrec: function(value, precision) {
    if(typeof value === "number") {
      let pow = Math.pow(10, precision);

      return Math.round(value * pow) / pow;
    }
    return "–";
  }
};
